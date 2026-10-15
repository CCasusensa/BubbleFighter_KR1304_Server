#include <algorithm>
#include <array>
#include <limits>

#include "persistence/inventory/inventory_rows.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"
#include "protocol/codec/packet_writer.h"
#include "protocol/inventory/item_use_response.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
using namespace inventory_sql;
using features::clans::ClanCreation;
namespace {
std::vector<std::uint8_t> TextBytes(std::u16string_view text) {
  protocol::PacketWriter writer;
  for (const auto unit : text) writer.WriteUInt16(unit);
  return writer.TakeBytes();
}
}  // namespace

protocol::ClanCreateResponse MySqlClanStore::Create(
    const features::clans::ClanIdentity& identity,
    const protocol::ClanCreateRequest& request,
    std::uint64_t observed_at_unix_seconds,
    const features::clans::ClanCreationAuthority& authorized) {
  if (!authorized || !authorized())
    throw std::runtime_error("Clan creation authorization expired.");
  const auto validation = ClanCreation::ValidateRequest(request);
  if (validation != 1) return {validation, 0, 0};
  if (observed_at_unix_seconds >= 4102444800ULL)
    throw std::runtime_error(
        "Clan creation time is outside the supported range.");
  auto* connection = connection_.Handle();
  Transaction transaction(connection);
  if (!LockOwner(connection, identity.account_id, identity.character_id))
    throw std::runtime_error("Clan creation owner is unavailable.");
  {
    Statement membership(
        connection,
        "SELECT MembershipStatus FROM ClanMembers WHERE CharacterId = ?");
    std::array params{Number(identity.character_id)};
    membership.Execute(params);
    std::uint32_t role = 0;
    std::array results{Number(role)};
    if (membership.Fetch(results))
      return {role == 1 ? 0xffffff98U : 0xffffff97U, 0, 0};
  }
  std::uint32_t level = 0;
  {
    Statement player(connection,
                     "SELECT Level FROM Characters WHERE CharacterId = ?");
    std::array params{Number(identity.character_id)};
    player.Execute(params);
    std::array results{Number(level)};
    if (!player.Fetch(results))
      throw std::runtime_error("Clan creation owner is unavailable.");
  }
  auto inventory =
      LoadSlots(connection, identity.account_id, identity.character_id);
  if (!ClanCreation::ConsumeCard(inventory, level, request.item_id,
                                 observed_at_unix_seconds))
    return {};
  auto& slot = *std::find_if(
      inventory.slots.begin(), inventory.slots.end(),
      [&](const auto& row) { return row.item_id == request.item_id; });
  protocol::ClanCreateResponse response{1, request.item_id, 0};
  response.Encode();
  // Validate the future confirmation snapshot before any write/commit.
  inventory.CreateNotification(identity.account_id, observed_at_unix_seconds)
      .Encode();
  const auto name = TextBytes(request.name);
  const auto introduction = TextBytes(request.introduction);
  {
    Statement insert(
        connection,
        "INSERT INTO Clans (NameUtf16, IntroductionUtf16, MemberCapacity, CreatedAtUnixSeconds) VALUES (?, ?, ?, ?)");
    std::array params{
        Bytes(name.data(), static_cast<unsigned long>(name.size())),
        Bytes(introduction.data(),
              static_cast<unsigned long>(introduction.size())),
        Number(request.item_parameter), Number(observed_at_unix_seconds)};
    try {
      insert.Execute(params);
    } catch (const MySqlError& error) {
      if (error.Code() == 1062) return {2, 0, 0};
      throw;
    }
    if (insert.AffectedRows() != 1)
      throw std::runtime_error("Clan creation insert failed.");
  }
  const auto generated = mysql_insert_id(connection);
  if (!generated || generated == 99 ||
      generated > (std::numeric_limits<std::uint32_t>::max)())
    throw std::runtime_error("Clan identifier exhausted.");
  const auto clan_id = static_cast<std::uint32_t>(generated);
  {
    Statement master(
        connection,
        "INSERT INTO ClanMembers (CharacterId, ClanId, MembershipStatus) VALUES (?, ?, 101)");
    std::array params{Number(identity.character_id), Number(clan_id)};
    master.Execute(params);
    if (master.AffectedRows() != 1)
      throw std::runtime_error("Clan creator membership insert failed.");
  }
  {
    Statement clear(connection,
                    "DELETE FROM ClanDisbandNotices WHERE CharacterId = ?");
    std::array params{Number(identity.character_id)};
    clear.Execute(params);
    Statement history(
        connection,
        "UPDATE TrainingClanHistory SET ExitStatus = 0 WHERE CharacterId = ?");
    history.Execute(params);
  }
  // PersistGrant only uses these trusted identifiers; this operation neither
  // renames a character nor adopts a stale main-session statistics cache.
  const features::characters::CharacterRecord owner{identity.character_id,
                                                    identity.account_id};
  PersistGrant(connection, owner, slot);
  {
    Statement receipt(
        connection,
        "INSERT INTO ClanCreationReceipts (ClanId, CharacterId, ItemId, InventoryRevision, CreatedAtUnixSeconds) VALUES (?, ?, ?, ?, ?)");
    std::array params{Number(clan_id), Number(identity.character_id),
                      Number(request.item_id), Number(slot.revision),
                      Number(observed_at_unix_seconds)};
    receipt.Execute(params);
    if (receipt.AffectedRows() != 1)
      throw std::runtime_error("Clan creation receipt insert failed.");
  }
  if (!authorized())
    throw std::runtime_error("Clan creation authorization expired.");
  transaction.Commit();
  return response;
}

std::vector<protocol::TcpPacket> MySqlClanStore::ConfirmCreation(
    const features::clans::ClanIdentity& identity,
    const protocol::ItemUseRequest& request,
    std::uint64_t observed_at_unix_seconds) {
  request.Encode();
  if (!ClanCreation::SupportsItem(request.item_id) || request.value_at4 != 1 ||
      !request.arguments.empty())
    throw std::runtime_error("Invalid clan creation confirmation.");
  auto* connection = connection_.Handle();
  Transaction transaction(connection);
  if (!LockOwner(connection, identity.account_id, identity.character_id))
    throw std::runtime_error("Clan creation owner is unavailable.");
  {
    Statement receipt(
        connection,
        "SELECT ClanId FROM ClanCreationReceipts WHERE CharacterId = ? AND ItemId = ? ORDER BY ClanId DESC LIMIT 1");
    std::array params{Number(identity.character_id), Number(request.item_id)};
    receipt.Execute(params);
    std::uint32_t clan = 0;
    std::array results{Number(clan)};
    if (!receipt.Fetch(results))
      throw std::runtime_error(
          "Clan creation confirmation has no committed receipt.");
  }
  auto inventory =
      LoadSlots(connection, identity.account_id, identity.character_id);
  std::vector<protocol::TcpPacket> packets{
      protocol::ItemUseResponse{0, request.item_id, {}}.Encode(),
      inventory
          .CreateNotification(identity.account_id, observed_at_unix_seconds)
          .Encode()};
  // A delayed/replayed C806 is a read of the latest inventory. It never debits
  // an item; the receipt was committed atomically with creation and
  // consumption.
  transaction.Commit();
  return packets;
}
}  // namespace bubble_fighter::server::persistence
