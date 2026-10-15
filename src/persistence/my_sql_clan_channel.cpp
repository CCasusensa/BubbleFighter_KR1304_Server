#include <array>
#include <set>

#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
std::optional<std::vector<features::clans::ClanChannelMember>>
MySqlClanStore::ReadChannelMembers(
    std::span<const features::clans::ClanIdentity> identities) {
  if (identities.empty() || identities.size() > 8) return {};
  std::set<std::uint64_t> accounts;
  std::set<std::uint32_t> characters;
  for (const auto& identity : identities) {
    if (!identity.account_id || !identity.character_id ||
        !accounts.insert(identity.account_id).second ||
        !characters.insert(identity.character_id).second)
      return {};
  }
  auto* connection = connection_.Handle();
  {
    Statement isolation(connection,
                        "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    isolation.Execute({});
  }
  Transaction transaction(connection);
  std::vector<features::clans::ClanChannelMember> result;
  result.reserve(identities.size());
  for (const auto& identity : identities) {
    Statement row(
        connection,
        "SELECT c.AccountId, c.CharacterId, c.Level, a.IsDisabled, COALESCE(m.ClanId, 0), COALESCE(m.MembershipStatus, 0) FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId LEFT JOIN ClanMembers m ON m.CharacterId = c.CharacterId WHERE c.CharacterId = ? AND c.AccountId = ?");
    std::array parameters{Number(identity.character_id),
                          Number(identity.account_id)};
    row.Execute(parameters);
    features::clans::ClanChannelMember member;
    std::uint8_t disabled = 0;
    std::array columns{Number(member.identity.account_id),
                       Number(member.identity.character_id),
                       Number(member.level),
                       Number(disabled),
                       Number(member.clan_id),
                       Number(member.membership_status)};
    if (!row.Fetch(columns)) return {};
    member.enabled = !disabled;
    result.push_back(member);
  }
  // No database transaction/row lock is retained by channel scene callbacks.
  transaction.Commit();
  return result;
}
}  // namespace bubble_fighter::server::persistence
