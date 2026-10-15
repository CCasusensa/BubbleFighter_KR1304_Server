#include <array>
#include <set>

#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"
#include "protocol/codec/packet_reader.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
std::optional<std::vector<features::clans::ClanPlazaMember>>
MySqlClanStore::ReadPlazaMembers(
    std::span<const features::clans::ClanIdentity> identities) {
  if (identities.empty() || identities.size() > 8) return {};
  std::set<std::uint64_t> accounts;
  std::set<std::uint32_t> characters;
  for (const auto& identity : identities)
    if (!identity.account_id || !identity.character_id ||
        !accounts.insert(identity.account_id).second ||
        !characters.insert(identity.character_id).second)
      return {};
  auto* connection = connection_.Handle();
  {
    Statement isolation(connection,
                        "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    isolation.Execute({});
  }
  Transaction transaction(connection);
  std::vector<features::clans::ClanPlazaMember> result;
  result.reserve(identities.size());
  for (const auto& identity : identities) {
    Statement row(
        connection,
        "SELECT c.AccountId, c.CharacterId, c.Level, a.IsDisabled, m.ClanId, m.MembershipStatus, g.NameUtf16 FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId JOIN ClanMembers m ON m.CharacterId = c.CharacterId JOIN Clans g ON g.ClanId = m.ClanId WHERE c.CharacterId = ? AND c.AccountId = ?");
    std::array parameters{Number(identity.character_id),
                          Number(identity.account_id)};
    row.Execute(parameters);
    features::clans::ClanPlazaMember value;
    auto& member = value.member;
    std::uint8_t disabled = 0;
    std::array<std::uint8_t, 24> name{};
    unsigned long size = 0;
    std::array columns{
        Number(member.identity.account_id),
        Number(member.identity.character_id),
        Number(member.level),
        Number(disabled),
        Number(member.clan_id),
        Number(member.membership_status),
        Bytes(name.data(), static_cast<unsigned long>(name.size()))};
    columns.back().length = &size;
    if (!row.Fetch(columns) || size > name.size() || size % 2) return {};
    protocol::PacketReader reader(std::span(name).first(size));
    while (reader.RemainingBytes())
      value.clan_name.push_back(reader.ReadUInt16());
    member.enabled = !disabled;
    result.push_back(std::move(value));
  }
  // Membership and canonical name share this snapshot; no DB locks escape it.
  transaction.Commit();
  return result;
}
}  // namespace bubble_fighter::server::persistence
