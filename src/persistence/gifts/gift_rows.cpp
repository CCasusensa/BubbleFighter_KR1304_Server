#include "persistence/gifts/gift_rows.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "features/characters/character_creation.h"
#include "persistence/battle_record_column.h"
#include "persistence/gifts/gift_bundle_rows.h"
#include "persistence/inventory/inventory_rows.h"
#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::persistence::gift_sql {
using namespace my_sql_commands;
using namespace features::gifts;
using features::characters::CharacterRecord;
namespace {
std::vector<std::uint8_t> BytesOf(std::u16string_view value) {
  protocol::PacketWriter writer;
  for (const auto unit : value) writer.WriteUInt16(unit);
  return writer.TakeBytes();
}
std::u16string TextOf(std::span<const std::uint8_t> value) {
  if (value.size() % 2) throw std::runtime_error("Invalid gift stored text.");
  protocol::PacketReader reader(value);
  std::u16string result;
  while (reader.RemainingBytes()) result.push_back(reader.ReadUInt16());
  return result;
}
}  // namespace

std::optional<CharacterRecord> FindRecipient(MYSQL* connection,
                                             std::u16string_view name) {
  if (features::characters::CharacterCreation::ValidateName(name)) return {};
  const auto bytes = BytesOf(name);
  Statement query(
      connection,
      "SELECT c.CharacterId, c.AccountId, c.CharacterType, c.ColorIndex, c.Level, c.Experience, c.Lucci, c.Cash, c.BattleRecords FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId WHERE c.NameUtf16 = ? AND a.IsDisabled = 0");
  std::array parameters{
      Bytes(bytes.data(), static_cast<unsigned long>(bytes.size()))};
  query.Execute(parameters);
  BattleRecordColumn battle;
  CharacterRecord result;
  result.character_name = name;
  std::array columns{Number(result.character_id),
                     Number(result.account_id),
                     Number(result.character_type),
                     Number(result.color_index),
                     Number(result.statistics.level),
                     Number(result.statistics.experience),
                     Number(result.statistics.lucci),
                     Number(result.statistics.cash),
                     battle.Bind()};
  if (!query.Fetch(columns)) return {};
  result.battle = battle.Read();
  return result;
}
void LockOwners(MYSQL* connection, const CharacterRecord& owner,
                const CharacterRecord& recipient) {
  const auto* first = &owner;
  const auto* second = &recipient;
  if (first->character_id > second->character_id) std::swap(first, second);
  if (!inventory_sql::LockOwner(connection, first->account_id,
                                first->character_id) ||
      (first->character_id != second->character_id &&
       !inventory_sql::LockOwner(connection, second->account_id,
                                 second->character_id)))
    throw std::runtime_error("Gift owner is unavailable.");
}

namespace {
std::vector<GiftRecord> LoadRows(MYSQL* connection, std::uint64_t owner,
                                 std::uint64_t other,
                                 std::uint64_t pending_at) {
  std::vector<GiftRecord> result;
  {
    // Visible operations keep existing row locks and retry rules. Admission
    // reads only recipient-owned pending rows under the recipient character
    // lock; sender-only hiding does not change these immutable delivery terms.
    Statement query(
        connection,
        pending_at
            ? "SELECT GiftId, SenderCharacterId, RecipientCharacterId, SenderNameUtf16, RecipientNameUtf16, StockId, ItemId, UsageKind, Amount, MaxAmount, Price, IsCash, SentAtUnixSeconds, HandledAtUnixSeconds, GiftState, SenderHidden, RecipientHidden, MessageUtf16, ThanksPayload FROM Gifts WHERE RecipientCharacterId = ? AND RecipientHidden = 0 AND GiftState = 0 AND SentAtUnixSeconds > ? ORDER BY GiftId LIMIT 201"
            : "SELECT GiftId, SenderCharacterId, RecipientCharacterId, SenderNameUtf16, RecipientNameUtf16, StockId, ItemId, UsageKind, Amount, MaxAmount, Price, IsCash, SentAtUnixSeconds, HandledAtUnixSeconds, GiftState, SenderHidden, RecipientHidden, MessageUtf16, ThanksPayload FROM Gifts WHERE (SenderCharacterId IN (?, ?) AND SenderHidden = 0) OR (RecipientCharacterId IN (?, ?) AND RecipientHidden = 0) ORDER BY GiftId LIMIT 801 FOR UPDATE");
    if (pending_at) {
      const auto cutoff = pending_at > kGiftLifetimeSeconds
                              ? pending_at - kGiftLifetimeSeconds
                              : 0;
      std::array parameters{Number(owner), Number(cutoff)};
      query.Execute(parameters);
    } else {
      std::array parameters{Number(owner), Number(other), Number(owner),
                            Number(other)};
      query.Execute(parameters);
    }
    GiftRecord gift;
    std::uint8_t usage = 0, cash = 0, state = 0, sender_hidden = 0,
                 recipient_hidden = 0;
    std::array<std::uint8_t, 32> sender_name{}, recipient_name{};
    std::array<std::uint8_t, kGiftMessageUnits * 2> message{};
    std::vector<std::uint8_t> thanks(65536);
    unsigned long sender_length = 0, recipient_length = 0, message_length = 0,
                  thanks_length = 0;
    bool thanks_null = true;
    std::array columns{
        Number(gift.gift_id),
        Number(gift.sender_id),
        Number(gift.recipient_id),
        Bytes(sender_name.data(),
              static_cast<unsigned long>(sender_name.size())),
        Bytes(recipient_name.data(),
              static_cast<unsigned long>(recipient_name.size())),
        Number(gift.stock_id),
        Number(gift.item_id),
        Number(usage),
        Number(gift.amount),
        Number(gift.max_amount),
        Number(gift.price),
        Number(cash),
        Number(gift.sent_at),
        Number(gift.handled_at),
        Number(state),
        Number(sender_hidden),
        Number(recipient_hidden),
        Bytes(message.data(), static_cast<unsigned long>(message.size())),
        Bytes(thanks.data(), static_cast<unsigned long>(thanks.size()))};
    columns[3].length = &sender_length;
    columns[4].length = &recipient_length;
    columns[17].length = &message_length;
    columns[18].length = &thanks_length;
    columns[18].is_null = &thanks_null;
    while (query.Fetch(columns)) {
      if (result.size() == (pending_at ? 1
                            : other    ? 4
                                       : 2) *
                               kGiftCapacity ||
          cash > 1 || sender_hidden > 1 || recipient_hidden > 1 ||
          sender_length > sender_name.size() ||
          recipient_length > recipient_name.size() ||
          message_length > message.size() ||
          (!thanks_null && thanks_length > thanks.size()))
        throw std::runtime_error("Invalid gift database snapshot.");
      gift.sender_name = TextOf(std::span(sender_name).first(sender_length));
      gift.recipient_name =
          TextOf(std::span(recipient_name).first(recipient_length));
      gift.message = TextOf(std::span(message).first(message_length));
      gift.usage = static_cast<features::inventory::InventoryUsage>(usage);
      gift.cash = cash != 0;
      gift.state = static_cast<GiftState>(state);
      gift.sender_hidden = sender_hidden != 0;
      gift.recipient_hidden = recipient_hidden != 0;
      gift.thanks_message.clear();
      gift.thanks_emotion = gift.thanks_color = 0;
      gift.thanks_appearance = {};
      if (!thanks_null) {
        const auto response = protocol::GiftViewThanksResponse::Decode(
            {protocol::GiftViewThanksResponse::kMessageId,
             {thanks.begin(), thanks.begin() + thanks_length}});
        if (response.value_at0 != 1 || response.gift_id != gift.gift_id ||
            response.text_at8 != gift.recipient_name)
          throw std::runtime_error("Invalid gift thanks ownership.");
        gift.thanks_message = response.text_at32;
        gift.thanks_emotion = response.value_at56;
        gift.thanks_color = response.value_at60;
        gift.thanks_appearance = response.extra;
      }
      result.push_back(gift);
    }
  }
  for (auto& row : result) {
    LoadBundle(connection, row);
    ValidateGift(row);
  }
  return result;
}

}  // namespace
std::vector<GiftRecord> LoadVisible(MYSQL* connection, std::uint64_t owner,
                                    std::uint64_t other) {
  return LoadRows(connection, owner, other, 0);
}
std::vector<GiftRecord> LoadPending(MYSQL* connection, std::uint64_t recipient,
                                    std::uint64_t now) {
  ValidateGiftTime(now);
  return LoadRows(connection, recipient, 0, now);
}
void Insert(MYSQL* connection, const GiftRecord& gift) {
  ValidateGift(gift);
  const auto sender_name = BytesOf(gift.sender_name);
  const auto recipient_name = BytesOf(gift.recipient_name);
  const auto message = BytesOf(gift.message);
  const auto usage = static_cast<std::uint8_t>(gift.usage);
  const std::uint8_t cash = gift.cash;
  Statement query(
      connection,
      "INSERT INTO Gifts (GiftId, SenderCharacterId, RecipientCharacterId, SenderNameUtf16, RecipientNameUtf16, StockId, ItemId, UsageKind, Amount, MaxAmount, Price, IsCash, SentAtUnixSeconds, MessageUtf16) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  std::array parameters{
      Number(gift.gift_id),
      Number(gift.sender_id),
      Number(gift.recipient_id),
      Bytes(sender_name.data(), static_cast<unsigned long>(sender_name.size())),
      Bytes(recipient_name.data(),
            static_cast<unsigned long>(recipient_name.size())),
      Number(gift.stock_id),
      Number(gift.item_id),
      Number(usage),
      Number(gift.amount),
      Number(gift.max_amount),
      Number(gift.price),
      Number(cash),
      Number(gift.sent_at),
      Bytes(message.data(), static_cast<unsigned long>(message.size()))};
  query.Execute(parameters);
  if (query.AffectedRows() != 1)
    throw std::runtime_error("Gift insert failed.");
  InsertBundle(connection, gift);
}
void Update(MYSQL* connection, const GiftRecord& gift) {
  ValidateGift(gift);
  std::vector<std::uint8_t> thanks;
  bool thanks_null = gift.thanks_emotion == 0;
  if (!thanks_null)
    thanks = protocol::GiftViewThanksResponse{1,
                                              gift.gift_id,
                                              gift.recipient_name,
                                              gift.thanks_message,
                                              gift.thanks_emotion,
                                              gift.thanks_color,
                                              gift.thanks_appearance}
                 .Encode()
                 .payload;
  if (thanks.size() > 65536)
    throw std::runtime_error("Gift appearance exceeds storage limit.");
  const auto state = static_cast<std::uint8_t>(gift.state);
  const std::uint8_t sender_hidden = gift.sender_hidden,
                     recipient_hidden = gift.recipient_hidden;
  Statement query(
      connection,
      "UPDATE Gifts SET GiftState = ?, HandledAtUnixSeconds = ?, SenderHidden = ?, RecipientHidden = ?, ThanksPayload = ? WHERE GiftId = ? AND SenderCharacterId = ? AND RecipientCharacterId = ?");
  std::array parameters{
      Number(state),
      Number(gift.handled_at),
      Number(sender_hidden),
      Number(recipient_hidden),
      Bytes(thanks.data(), static_cast<unsigned long>(thanks.size())),
      Number(gift.gift_id),
      Number(gift.sender_id),
      Number(gift.recipient_id)};
  parameters[4].is_null = &thanks_null;
  query.Execute(parameters);
  if (query.AffectedRows() != 1)
    throw std::runtime_error("Gift record changed.");
}
}  // namespace bubble_fighter::server::persistence::gift_sql
