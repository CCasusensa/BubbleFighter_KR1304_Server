#include <algorithm>
#include <array>
#include <map>

#include "features/characters/character_creation.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"
#include "protocol/codec/packet_reader.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
using namespace features::clans;
using namespace protocol;
namespace {
void ReadCommitted(MYSQL* connection) {
  Statement isolation(connection,
                      "SET TRANSACTION ISOLATION LEVEL READ COMMITTED");
  isolation.Execute({});
}
std::u16string Name(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || bytes.size() % 2)
    throw std::runtime_error("Invalid clan transfer state.");
  PacketReader reader(bytes);
  std::u16string text;
  while (reader.RemainingBytes()) text.push_back(reader.ReadUInt16());
  if (features::characters::CharacterCreation::ValidateName(text))
    throw std::runtime_error("Invalid clan transfer state.");
  return text;
}
void Deliver(ClanMutationResult& result, std::vector<std::uint32_t> targets,
             std::vector<ClanPacket> packets) {
  if (targets.empty()) return;
  for (const auto& packet : packets) ClanPacketCodec::Encode(packet);
  std::sort(targets.begin(), targets.end());
  result.deliveries.push_back(
      {std::move(targets),
       std::make_shared<const std::vector<ClanPacket>>(std::move(packets))});
}
}  // namespace

std::optional<ClanMasterTransferParties> MySqlClanStore::ReadMasterTransfer(
    const ClanIdentity& requester, std::uint32_t target) {
  if (!requester.account_id || !requester.character_id || !target ||
      requester.character_id == target)
    return {};
  auto* connection = connection_.Handle();
  struct Character {
    std::uint64_t account = 0;
    std::u16string name;
  };
  std::map<std::uint32_t, Character> characters;
  std::array ids{requester.character_id, target};
  std::sort(ids.begin(), ids.end());
  // Same lock order as ordinary mutations: sorted characters/accounts, clan.
  for (const auto id : ids) {
    Statement query(
        connection,
        "SELECT c.AccountId, c.NameUtf16, a.IsDisabled FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId WHERE c.CharacterId = ? FOR UPDATE");
    std::array params{Number(id)};
    query.Execute(params);
    Character row;
    std::uint8_t disabled = 0;
    std::array<std::uint8_t, 32> bytes{};
    unsigned long length = 0;
    std::array fields{
        Number(row.account),
        Bytes(bytes.data(), static_cast<unsigned long>(bytes.size())),
        Number(disabled)};
    fields[1].length = &length;
    if (!query.Fetch(fields) || disabled) return {};
    if (length > bytes.size())
      throw std::runtime_error("Invalid clan transfer state.");
    row.name = Name(std::span(bytes).first(length));
    characters.emplace(id, std::move(row));
  }
  if (characters.at(requester.character_id).account != requester.account_id)
    return {};
  std::uint32_t clan = 0, role = 0, target_clan = 0, target_role = 0;
  {
    Statement query(
        connection,
        "SELECT ClanId, MembershipStatus FROM ClanMembers WHERE CharacterId = ?");
    std::array params{Number(requester.character_id)};
    query.Execute(params);
    std::array fields{Number(clan), Number(role)};
    if (!query.Fetch(fields) || role != 101 || !clan || clan == 99) return {};
  }
  {
    Statement query(
        connection,
        "SELECT DisbandAtUnixSeconds FROM Clans WHERE ClanId = ? FOR UPDATE");
    std::array params{Number(clan)};
    query.Execute(params);
    std::uint64_t disband = 0;
    std::array fields{Number(disband)};
    if (!query.Fetch(fields) || disband) return {};
  }
  {
    Statement query(
        connection,
        "SELECT ClanId, MembershipStatus FROM ClanMembers WHERE CharacterId = ?");
    std::array params{Number(target)};
    query.Execute(params);
    std::array fields{Number(target_clan), Number(target_role)};
    if (!query.Fetch(fields) || target_clan != clan ||
        (target_role != 100 && target_role != 102))
      return {};
  }
  return ClanMasterTransferParties{requester,
                                   {characters.at(target).account, target},
                                   clan,
                                   characters.at(requester.character_id).name,
                                   characters.at(target).name,
                                   target_role};
}
std::optional<ClanMasterTransferParties> MySqlClanStore::PrepareMasterTransfer(
    const ClanIdentity& requester, std::uint32_t target) {
  ReadCommitted(connection_.Handle());
  Transaction transaction(connection_.Handle());
  auto parties = ReadMasterTransfer(requester, target);
  transaction.Commit();
  return parties;
}
ClanMutationResult MySqlClanStore::CommitMasterTransfer(
    const ClanMasterTransferParties& parties,
    const ClanMutationAuthority& authorized) {
  if (!authorized || !authorized())
    throw std::runtime_error("Clan master transfer authorization expired.");
  auto* connection = connection_.Handle();
  ReadCommitted(connection);
  Transaction transaction(connection);
  const auto current =
      ReadMasterTransfer(parties.requester, parties.target.character_id);
  if (current != std::optional{parties}) {
    const auto snapshot = LoadInTransaction(parties.requester);
    if (!snapshot)
      throw std::runtime_error("Clan master transfer owner is unavailable.");
    return {
        {ClanMasterTransferResponse{0, 0}.Encode(), snapshot->profile.Encode()},
        {}};
  }
  std::vector<std::uint32_t> others;
  {
    Statement query(
        connection,
        "SELECT CharacterId FROM ClanMembers WHERE ClanId = ? AND MembershipStatus IN (100,101,102) ORDER BY CharacterId LIMIT 65536");
    std::array params{Number(parties.clan_id)};
    query.Execute(params);
    std::uint64_t id = 0;
    std::array fields{Number(id)};
    std::size_t count = 0;
    while (query.Fetch(fields)) {
      if (++count > 65535 || !id || id > 0xffffffffULL)
        throw std::runtime_error("Invalid clan transfer state.");
      if (id != parties.requester.character_id &&
          id != parties.target.character_id)
        others.push_back(static_cast<std::uint32_t>(id));
    }
  }
  const auto update = [&](std::uint32_t id, std::uint32_t before,
                          std::uint32_t after) {
    Statement query(
        connection,
        "UPDATE ClanMembers SET MembershipStatus = ? WHERE CharacterId = ? AND ClanId = ? AND MembershipStatus = ?");
    std::array params{Number(after), Number(id), Number(parties.clan_id),
                      Number(before)};
    query.Execute(params);
    if (query.AffectedRows() != 1)
      throw std::runtime_error("Clan master transfer write failed.");
  };
  // Free the unique master officer slot before assigning it. Both updates and
  // every prepared packet remain inside this transaction until authorization.
  update(parties.requester.character_id, 101, 100);
  update(parties.target.character_id, parties.target_status, 101);
  const auto former = LoadInTransaction(parties.requester);
  const auto next = LoadInTransaction(parties.target);
  if (!former || !next || !former->details || !next->details)
    throw std::runtime_error("Invalid clan transfer state.");
  const auto notification =
      ClanMasterTransferredNotification{
          0, parties.requester.character_id, parties.requester_name,
          parties.target.character_id, parties.target_name}
          .Encode();
  ClanMutationResult result{
      {notification, former->profile.Encode(), former->details->Encode()}, {}};
  Deliver(result, {parties.target.character_id},
          {notification, next->profile.Encode(), next->details->Encode()});
  Deliver(result, std::move(others), {notification});
  for (const auto& packet : result.responses) ClanPacketCodec::Encode(packet);
  if (!authorized())
    throw std::runtime_error("Clan master transfer authorization expired.");
  transaction.Commit();
  return result;
}
}  // namespace bubble_fighter::server::persistence
