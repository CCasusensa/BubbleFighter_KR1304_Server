#include <array>

#include "features/characters/character_creation.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"
#include "protocol/codec/packet_reader.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
using namespace features::clans;
using namespace protocol;

TrainingClanResult MySqlClanStore::Training(
    const ClanIdentity& identity, TrainingClanAction action, std::uint64_t now,
    const ClanMutationAuthority& authorized) {
  if (!authorized || !authorized() || !identity.account_id ||
      !identity.character_id || !now || now >= 4102444800ULL)
    throw std::runtime_error("Training clan authority or time is unavailable.");
  auto* connection = connection_.Handle();
  {
    Statement isolation(connection,
                        "SET TRANSACTION ISOLATION LEVEL READ COMMITTED");
    isolation.Execute({});
  }
  Transaction transaction(connection);
  std::u16string character_name;
  std::uint32_t level = 0;
  {
    Statement owner(
        connection,
        "SELECT c.AccountId, a.IsDisabled, c.Level, c.NameUtf16 FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId WHERE c.CharacterId = ? FOR UPDATE");
    std::array params{Number(identity.character_id)};
    owner.Execute(params);
    std::uint64_t account = 0;
    std::uint8_t disabled = 0;
    std::array<std::uint8_t, 32> bytes{};
    unsigned long length = 0;
    std::array fields{
        Number(account), Number(disabled), Number(level),
        Bytes(bytes.data(), static_cast<unsigned long>(bytes.size()))};
    fields[3].length = &length;
    if (!owner.Fetch(fields) || account != identity.account_id || disabled ||
        !level || !length || length > bytes.size() || length % 2)
      throw std::runtime_error("Training clan owner is unavailable.");
    PacketReader reader(std::span(bytes).first(length));
    while (reader.RemainingBytes())
      character_name.push_back(reader.ReadUInt16());
    if (features::characters::CharacterCreation::ValidateName(character_name))
      throw std::runtime_error("Invalid training clan state.");
  }
  std::uint32_t clan = 0, role = 0;
  {
    Statement query(
        connection,
        "SELECT ClanId, MembershipStatus FROM ClanMembers WHERE CharacterId = ?");
    std::array params{Number(identity.character_id)};
    query.Execute(params);
    std::array fields{Number(clan), Number(role)};
    query.Fetch(fields);
  }
  std::uint64_t changed_at = 0;
  std::uint32_t exit_status = 0;
  std::uint8_t graduated = 0;
  {
    Statement query(
        connection,
        "SELECT LastChangedAtUnixSeconds, ExitStatus, Graduated FROM TrainingClanHistory WHERE CharacterId = ?");
    std::array params{Number(identity.character_id)};
    query.Execute(params);
    std::array fields{Number(changed_at), Number(exit_status),
                      Number(graduated)};
    query.Fetch(fields);
  }
  if (clan == 99 && (role != 100 || !changed_at || exit_status || graduated))
    throw std::runtime_error("Invalid training clan state.");
  TrainingClanResult result;
  result.graduated = graduated != 0;
  const auto profile = [&] {
    const auto snapshot = LoadInTransaction(identity);
    if (!snapshot)
      throw std::runtime_error("Training clan owner is unavailable.");
    return snapshot->profile.Encode();
  };
  const auto refuse = [&] {
    if (action == TrainingClanAction::kJoin)
      result.mutation.responses.push_back(TrainingClanJoinResponse{}.Encode());
    if (action != TrainingClanAction::kReconcile)
      result.mutation.responses.push_back(profile());
    return result;
  };
  const bool joining = action == TrainingClanAction::kJoin;
  const bool graduate = clan == 99 && level > 45 &&
                        (action == TrainingClanAction::kReconcile ||
                         action == TrainingClanAction::kGraduate ||
                         action == TrainingClanAction::kManualLeave);
  if (joining) {
    if (clan || level > 45 || graduated ||
        (changed_at && (now < changed_at || now - changed_at < 60)))
      return refuse();
  } else if (action == TrainingClanAction::kManualLeave && clan != 99) {
    result.handled = false;  // Native C1036 may belong to an ordinary clan.
    return refuse();
  } else if (!graduate &&
             (action != TrainingClanAction::kManualLeave || clan != 99)) {
    return refuse();
  }
  if (now < changed_at) return refuse();
  // All mutations lock character/account before the shared clan row. Service
  // operation serialization and this row lock also protect first creation.
  if (joining) {
    Statement seed(
        connection,
        "INSERT INTO Clans (ClanId, NameUtf16, IntroductionUtf16, MemberCapacity, MarkId, ClanLevel, ClanPoints, IsRecruiting, CreatedAtUnixSeconds, DisbandAtUnixSeconds) VALUES (99, X'C8D628B88CC1', X'C8D628B88CC1', 65535, 49, 1, 0, 2, 0, 0) ON DUPLICATE KEY UPDATE ClanId = ClanId");
    seed.Execute({});
  }
  {
    Statement lock(
        connection,
        "SELECT ClanId FROM Clans WHERE ClanId = 99 AND NameUtf16 = X'C8D628B88CC1' AND MemberCapacity = 65535 AND MarkId = 49 AND ClanLevel = 1 AND ClanPoints = 0 AND IsRecruiting = 2 AND CreatedAtUnixSeconds = 0 AND DisbandAtUnixSeconds = 0 FOR UPDATE");
    lock.Execute({});
    std::uint32_t id = 0;
    std::array fields{Number(id)};
    if (!lock.Fetch(fields) || id != 99)
      throw std::runtime_error("Invalid training clan state.");
  }
  if (joining) {
    {
      Statement count(connection,
                      "SELECT COUNT(*) FROM ClanMembers WHERE ClanId = 99");
      count.Execute({});
      std::uint64_t members = 0;
      std::array fields{Number(members)};
      if (!count.Fetch(fields) || members >= 65535) return refuse();
    }
    Statement insert(
        connection,
        "INSERT INTO ClanMembers (CharacterId, ClanId, MembershipStatus) VALUES (?, 99, 100)");
    std::array params{Number(identity.character_id)};
    insert.Execute(params);
    if (insert.AffectedRows() != 1)
      throw std::runtime_error("Training clan write failed.");
  } else {
    Statement erase(
        connection,
        "DELETE FROM ClanMembers WHERE CharacterId = ? AND ClanId = 99 AND MembershipStatus = 100");
    std::array params{Number(identity.character_id)};
    erase.Execute(params);
    if (erase.AffectedRows() != 1)
      throw std::runtime_error("Training clan write failed.");
  }
  const std::uint32_t next = joining    ? 0U
                             : graduate ? 0xffffffc4U
                                        : 0xffffffecU;
  {
    Statement history(
        connection,
        "INSERT INTO TrainingClanHistory (CharacterId, LastChangedAtUnixSeconds, ExitStatus, Graduated) VALUES (?, ?, ?, ?) ON DUPLICATE KEY UPDATE LastChangedAtUnixSeconds = ?, ExitStatus = ?, Graduated = ?");
    const std::uint8_t final_graduated = graduate ? 1U : 0U;
    std::array params{Number(identity.character_id), Number(now), Number(next),
                      Number(final_graduated),       Number(now), Number(next),
                      Number(final_graduated)};
    history.Execute(params);
  }
  {
    Statement clear(connection,
                    "DELETE FROM ClanDisbandNotices WHERE CharacterId = ?");
    std::array params{Number(identity.character_id)};
    clear.Execute(params);
  }
  if (joining)
    result.mutation.responses.push_back(
        TrainingClanJoinResponse{99, 100, u"훈련소"}.Encode());
  else
    result.mutation.responses.push_back(ClanLeaveNotification{
        u"훈련소", identity.character_id, character_name, 100, next}
                                            .Encode());
  result.mutation.responses.push_back(profile());
  result.graduated = graduate;
  for (const auto& packet : result.mutation.responses)
    ClanPacketCodec::Encode(packet);
  if (!authorized())
    throw std::runtime_error("Training clan authorization expired.");
  transaction.Commit();
  return result;
}
}  // namespace bubble_fighter::server::persistence
