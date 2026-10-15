#include <array>

#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
using namespace features::clans;
using namespace protocol;
namespace {
constexpr std::uint64_t kDateLimit = 4102444800ULL;
void ReadCommitted(MYSQL* connection) {
  Statement isolation(connection,
                      "SET TRANSACTION ISOLATION LEVEL READ COMMITTED");
  isolation.Execute({});
}
void Deliver(ClanMutationResult& result, std::vector<std::uint32_t> targets,
             std::vector<ClanPacket> packets) {
  if (targets.empty()) return;
  for (const auto& packet : packets) ClanPacketCodec::Encode(packet);
  result.deliveries.push_back(
      {std::move(targets),
       std::make_shared<const std::vector<ClanPacket>>(std::move(packets))});
}
}  // namespace

ClanMutationResult MySqlClanStore::Disband(
    const ClanIdentity& identity, const ClanDisbandRequest& request,
    std::uint64_t now, std::uint64_t maintenance,
    const ClanMutationAuthority& authorized) {
  if (!authorized || !authorized())
    throw std::runtime_error("Clan disband authorization expired.");
  auto* connection = connection_.Handle();
  ReadCommitted(connection);
  Transaction transaction(connection);
  // Match the live mutation lock order: character/account, then clan.
  {
    Statement owner(
        connection,
        "SELECT c.AccountId, a.IsDisabled FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId WHERE c.CharacterId = ? FOR UPDATE");
    std::array params{Number(identity.character_id)};
    owner.Execute(params);
    std::uint64_t account = 0;
    std::uint8_t disabled = 0;
    std::array fields{Number(account), Number(disabled)};
    if (!identity.account_id || !identity.character_id ||
        !owner.Fetch(fields) || account != identity.account_id || disabled)
      throw std::runtime_error("Clan disband owner is unavailable.");
  }
  std::uint32_t clan = 0, role = 0;
  {
    Statement member(
        connection,
        "SELECT ClanId, MembershipStatus FROM ClanMembers WHERE CharacterId = ?");
    std::array params{Number(identity.character_id)};
    member.Execute(params);
    std::array fields{Number(clan), Number(role)};
    member.Fetch(fields);
  }
  const auto refuse = [&]() -> ClanMutationResult {
    const auto snapshot = LoadInTransaction(identity);
    if (!snapshot)
      throw std::runtime_error("Clan disband owner is unavailable.");
    return {{ClanDisbandResponse{0, request.action}.Encode(),
             snapshot->profile.Encode()},
            {}};
  };
  if (!clan || clan == 99 || role != 101 ||
      (request.action != 1 && request.action != 2) || !now || now >= kDateLimit)
    return refuse();
  std::uint64_t created = 0, due = 0;
  {
    Statement query(
        connection,
        "SELECT CreatedAtUnixSeconds, DisbandAtUnixSeconds FROM Clans WHERE ClanId = ? FOR UPDATE");
    std::array params{Number(clan)};
    query.Execute(params);
    std::array fields{Number(created), Number(due)};
    if (!query.Fetch(fields)) return refuse();
  }
  if ((request.action == 1 &&
       (due || maintenance <= now || maintenance < created ||
        maintenance >= kDateLimit)) ||
      (request.action == 2 && (!due || due <= now)))
    return refuse();
  const auto next = request.action == 1 ? maintenance : std::uint64_t{0};
  {
    Statement update(
        connection,
        "UPDATE Clans SET DisbandAtUnixSeconds = ? WHERE ClanId = ? AND DisbandAtUnixSeconds = ?");
    std::array params{Number(next), Number(clan), Number(due)};
    update.Execute(params);
    if (update.AffectedRows() != 1)
      throw std::runtime_error("Clan disband write failed.");
  }
  const auto snapshot = LoadInTransaction(identity);
  if (!snapshot || !snapshot->details)
    throw std::runtime_error("Invalid clan disband state.");
  const auto event = ClanDisbandNotification{1, request.action}.Encode();
  ClanMutationResult result{{ClanDisbandResponse{1, request.action}.Encode(),
                             event, snapshot->profile.Encode()},
                            {}};
  std::vector<std::uint32_t> accepted, applicants;
  {
    Statement query(
        connection,
        "SELECT CharacterId, MembershipStatus FROM ClanMembers WHERE ClanId = ? ORDER BY CharacterId LIMIT 66037");
    std::array params{Number(clan)};
    query.Execute(params);
    std::uint64_t id = 0;
    std::uint32_t status = 0;
    std::array fields{Number(id), Number(status)};
    std::size_t count = 0;
    while (query.Fetch(fields)) {
      if (++count > 66036 || !id || id > 0xffffffffULL ||
          (status != 1 && status != 100 && status != 101 && status != 102))
        throw std::runtime_error("Invalid clan disband state.");
      if (id != identity.character_id)
        (status == 1 ? applicants : accepted)
            .push_back(static_cast<std::uint32_t>(id));
    }
  }
  Deliver(result, std::move(accepted), {event});
  auto pending = snapshot->profile;
  pending.details_available = 0;
  pending.membership_status = 1;
  Deliver(result, std::move(applicants), {pending.Encode()});
  for (const auto& packet : result.responses) ClanPacketCodec::Encode(packet);
  if (!authorized())
    throw std::runtime_error("Clan disband authorization expired.");
  transaction.Commit();
  return result;
}

void MySqlClanStore::LockService() {
  // The connection owns this database-scoped lock until its destruction.
  Statement lock(
      connection_.Handle(),
      "SELECT GET_LOCK(CONCAT('bf_clan_', SHA2(DATABASE(), 224)), 0)");
  lock.Execute({});
  std::uint32_t acquired = 0;
  std::array fields{Number(acquired)};
  if (!lock.Fetch(fields) || acquired != 1)
    throw std::runtime_error("Clan service or maintenance is already running.");
}

std::uint64_t MySqlClanStore::DisbandDue(std::uint64_t now) {
  if (!now || now >= kDateLimit)
    throw std::invalid_argument("Invalid clan maintenance time.");
  auto* connection = connection_.Handle();
  ReadCommitted(connection);
  Transaction transaction(connection);
  std::vector<std::uint32_t> due;
  {
    Statement query(
        connection,
        "SELECT ClanId FROM Clans WHERE ClanId <> 99 AND DisbandAtUnixSeconds > 0 AND DisbandAtUnixSeconds <= ? ORDER BY ClanId LIMIT 10001 FOR UPDATE");
    std::array params{Number(now)};
    query.Execute(params);
    std::uint32_t id = 0;
    std::array fields{Number(id)};
    while (query.Fetch(fields)) {
      if (!id || due.size() >= 10000)
        throw std::runtime_error("Clan maintenance batch limit exceeded.");
      due.push_back(id);
    }
  }
  // Maintenance is an explicit offline command. Any SQL failure rolls back
  // the entire batch, including notices. Creation receipts deliberately remain.
  for (const auto clan : due) {
    {
      Statement notice(
          connection,
          "INSERT INTO ClanDisbandNotices (CharacterId, ClanId, NameUtf16, DisbandedAtUnixSeconds) SELECT m.CharacterId, c.ClanId, c.NameUtf16, ? FROM ClanMembers m JOIN Clans c ON c.ClanId = m.ClanId WHERE c.ClanId = ?");
      std::array params{Number(now), Number(clan)};
      notice.Execute(params);
    }
    Statement erase(
        connection,
        "DELETE FROM Clans WHERE ClanId = ? AND DisbandAtUnixSeconds > 0 AND DisbandAtUnixSeconds <= ?");
    std::array params{Number(clan), Number(now)};
    erase.Execute(params);
    if (erase.AffectedRows() != 1)
      throw std::runtime_error("Clan maintenance write failed.");
  }
  transaction.Commit();
  return due.size();
}
}  // namespace bubble_fighter::server::persistence
