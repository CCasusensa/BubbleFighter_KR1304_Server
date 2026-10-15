#include "persistence/my_sql_gift_store.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "persistence/gifts/gift_rows.h"
#include "persistence/inventory/inventory_rows.h"

namespace bubble_fighter::server::persistence {
using namespace features::gifts;
using namespace my_sql_commands;
using namespace inventory_sql;
using features::characters::CharacterRecord;

MySqlGiftStore::MySqlGiftStore(const std::filesystem::path& configuration)
    : connection_(configuration) {
  {
    Statement query(
        connection_.Handle(),
        "SELECT GiftId, SenderCharacterId, RecipientCharacterId, SenderNameUtf16, RecipientNameUtf16, StockId, ItemId, UsageKind, Amount, MaxAmount, Price, IsCash, SentAtUnixSeconds, HandledAtUnixSeconds, GiftState, SenderHidden, RecipientHidden, MessageUtf16, ThanksPayload FROM Gifts WHERE 1 = 0");
    query.Execute({});
  }
  {
    Statement query(
        connection_.Handle(),
        "SELECT b.GiftId, b.BonusLucci, b.ComponentCount, c.ComponentIndex, c.StockId, c.ItemId, c.UsageKind, c.Amount, c.MaxAmount FROM GiftBundles b LEFT JOIN GiftBundleComponents c ON c.GiftId = b.GiftId WHERE 1 = 0");
    query.Execute({});
  }
  {
    Statement query(connection_.Handle(),
                    "SELECT NextGiftId FROM GiftSequence WHERE SequenceId = 1");
    query.Execute({});
    std::uint32_t id = 0;
    std::array columns{Number(id)};
    if (!query.Fetch(columns) || id == 0 ||
        id > std::uint64_t{kGiftMaximumId} + 1)
      throw std::runtime_error("Invalid gift sequence.");
  }
}
GiftResult MySqlGiftStore::Execute(const CharacterRecord& owner,
                                   const GiftRequest& request,
                                   std::uint64_t now) {
  ValidateGiftTime(now);
  for (unsigned attempt = 0;; ++attempt) {
    try {
      return ExecuteTransaction(owner, request, now);
    } catch (const MySqlError& error) {
      // InnoDB has rolled back the deadlock victim; the transaction object's
      // destructor also restores autocommit. Never retry an ambiguous commit.
      if (error.Code() != 1213 || attempt == 2) throw;
    }
  }
}
GiftResult MySqlGiftStore::ExecuteTransaction(const CharacterRecord& owner,
                                              const GiftRequest& request,
                                              std::uint64_t now) {
  auto* connection = connection_.Handle();
  const auto* send = std::get_if<protocol::GiftSendRequest>(&request);
  auto recipient =
      send ? gift_sql::FindRecipient(connection, send->recipient_name)
           : std::optional<CharacterRecord>{};
  Transaction transaction(connection);
  if (recipient)
    gift_sql::LockOwners(connection, owner, *recipient);
  else if (!LockOwner(connection, owner.account_id, owner.character_id))
    throw std::runtime_error("Gift owner is unavailable.");
  const auto current = ReadCurrentCharacter(connection, owner);
  if (send) {
    if (!recipient) return RefuseGiftSend(current, -1);
    // The name is resolved again after ordered owner locks, preventing rename
    // races from redirecting a gift to a different account.
    const auto refreshed =
        gift_sql::FindRecipient(connection, send->recipient_name);
    if (!refreshed || refreshed->character_id != recipient->character_id)
      return RefuseGiftSend(current, -1);
    recipient = ReadCurrentCharacter(connection, *refreshed);
    if (recipient->character_id == current.character_id)
      return RefuseGiftSend(current, -18);
    {
      Statement blocked(
          connection,
          "SELECT COUNT(*) FROM SocialChatBlocks WHERE (OwnerCharacterId = ? AND TargetCharacterId = ?) OR (OwnerCharacterId = ? AND TargetCharacterId = ?)");
      std::array parameters{
          Number(current.character_id), Number(recipient->character_id),
          Number(recipient->character_id), Number(current.character_id)};
      blocked.Execute(parameters);
      std::uint64_t count = 0;
      std::array columns{Number(count)};
      if (!blocked.Fetch(columns))
        throw std::runtime_error("Missing gift block count.");
      if (count) return RefuseGiftSend(current, -77);
    }
    auto rows = gift_sql::LoadVisible(connection, current.character_id,
                                      recipient->character_id);
    const auto sent_count = static_cast<std::size_t>(
        std::count_if(rows.begin(), rows.end(), [&](const auto& row) {
          return row.sender_id == current.character_id && !row.sender_hidden;
        }));
    std::erase_if(rows, [&](const auto& row) {
      return !(row.sender_id == recipient->character_id &&
               !row.sender_hidden) &&
             !(row.recipient_id == recipient->character_id &&
               !row.recipient_hidden);
    });
    const auto inventory =
        LoadSlots(connection, recipient->account_id, recipient->character_id);
    const auto cash = LoadCashSlots(connection, recipient->account_id,
                                    recipient->character_id);
    std::uint32_t id = 0;
    {
      Statement query(
          connection,
          "SELECT NextGiftId FROM GiftSequence WHERE SequenceId = 1 FOR UPDATE");
      query.Execute({});
      std::array columns{Number(id)};
      if (!query.Fetch(columns))
        throw std::runtime_error("Missing gift sequence.");
    }
    auto result = PrepareGiftSend(*send, current, *recipient, inventory, cash,
                                  rows, sent_count, id, now);
    if (!result.changed) return result;
    const auto& gift = *result.changed;
    if (gift.price) {
      Statement debit(
          connection,
          gift.cash
              ? "UPDATE Characters SET Cash = ? WHERE CharacterId = ? AND AccountId = ? AND Cash = ?"
              : "UPDATE Characters SET Lucci = ? WHERE CharacterId = ? AND AccountId = ? AND Lucci = ?");
      const auto before =
          gift.cash ? current.statistics.cash : current.statistics.lucci;
      const auto after = gift.cash ? result.character.statistics.cash
                                   : result.character.statistics.lucci;
      std::array parameters{Number(after), Number(current.character_id),
                            Number(current.account_id), Number(before)};
      debit.Execute(parameters);
      if (debit.AffectedRows() != 1)
        throw std::runtime_error("Gift balance changed.");
    }
    gift_sql::Insert(connection, gift);
    {
      Statement query(
          connection,
          "UPDATE GiftSequence SET NextGiftId = NextGiftId + 1 WHERE SequenceId = 1 AND NextGiftId = ?");
      std::array parameters{Number(id)};
      query.Execute(parameters);
      if (query.AffectedRows() != 1)
        throw std::runtime_error("Gift sequence changed.");
    }
    transaction.Commit();
    return result;
  }
  const auto rows = gift_sql::LoadVisible(connection, current.character_id);
  const auto inventory =
      LoadSlots(connection, current.account_id, current.character_id);
  auto result = PrepareGiftAction(current, inventory, rows, request, now);
  if (!result.changed) return result;
  if (result.inventory) {
    for (auto& slot : result.inventory->slots) {
      const auto before = std::find_if(
          inventory.slots.begin(), inventory.slots.end(),
          [&](const auto& row) { return row.item_id == slot.item_id; });
      if (before == inventory.slots.end() || *before != slot)
        PersistGrant(connection, current, slot);
    }
  }
  if (result.character.statistics.lucci != current.statistics.lucci) {
    Statement credit(
        connection,
        "UPDATE Characters SET Lucci = ? WHERE CharacterId = ? AND AccountId = ? AND Lucci = ?");
    std::array parameters{
        Number(result.character.statistics.lucci), Number(current.character_id),
        Number(current.account_id), Number(current.statistics.lucci)};
    credit.Execute(parameters);
    if (credit.AffectedRows() != 1)
      throw std::runtime_error("Gift bonus balance changed.");
  }
  gift_sql::Update(connection, *result.changed);
  transaction.Commit();
  return result;
}
std::uint32_t MySqlGiftStore::Pending(const CharacterRecord& owner,
                                      std::uint64_t now) {
  ValidateGiftTime(now);
  auto* connection = connection_.Handle();
  Transaction transaction(connection);
  if (!LockOwner(connection, owner.account_id, owner.character_id))
    throw std::runtime_error("Gift owner is unavailable.");
  (void)ReadCurrentCharacter(connection, owner);
  Statement query(
      connection,
      "SELECT COUNT(*) FROM Gifts WHERE RecipientCharacterId = ? AND RecipientHidden = 0 AND GiftState = 0 AND SentAtUnixSeconds <= ? AND SentAtUnixSeconds > ?");
  const auto cutoff =
      now > kGiftLifetimeSeconds ? now - kGiftLifetimeSeconds : 0;
  std::array parameters{Number(owner.character_id), Number(now),
                        Number(cutoff)};
  query.Execute(parameters);
  std::uint64_t count = 0;
  std::array columns{Number(count)};
  if (!query.Fetch(columns) || count > kGiftCapacity)
    throw std::runtime_error("Invalid pending gift count.");
  return static_cast<std::uint32_t>(count);
}
}  // namespace bubble_fighter::server::persistence
