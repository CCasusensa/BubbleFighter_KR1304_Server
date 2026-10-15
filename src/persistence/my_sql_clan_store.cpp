#include "persistence/my_sql_clan_store.h"

#include <array>
#include <bit>

#include "features/characters/character_creation.h"
#include "persistence/my_sql_commands.h"
#include "protocol/codec/packet_reader.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
using features::clans::ClanIdentity;
using features::clans::ClanSnapshot;
namespace {
constexpr std::uint64_t kDateSentinel = 4102444800ULL;  // 2100-01-01 UTC.
std::uint64_t DateBits(std::uint64_t unix_seconds) {
  if (unix_seconds > kDateSentinel)
    throw std::runtime_error("Clan date is outside the supported range.");
  // Project UTC mapping into the native OLE DATE representation.
  return std::bit_cast<std::uint64_t>(
      25569.0 + static_cast<double>(unix_seconds) / 86400.0);
}
std::u16string Text(std::span<const std::uint8_t> bytes) {
  if (bytes.size() % 2) throw std::runtime_error("Malformed stored clan text.");
  protocol::PacketReader reader(bytes);
  std::u16string value;
  while (reader.RemainingBytes()) {
    const auto unit = reader.ReadUInt16();
    if (!unit) throw std::runtime_error("Malformed stored clan text.");
    value.push_back(unit);
  }
  return value;
}
}  // namespace

MySqlClanStore::MySqlClanStore(const std::filesystem::path& configuration)
    : connection_(configuration) {
  {
    Statement clans(
        connection_.Handle(),
        "SELECT ClanId, NameUtf16, IntroductionUtf16, MemberCapacity, MarkId, ClanLevel, ClanPoints, IsRecruiting, CreatedAtUnixSeconds, DisbandAtUnixSeconds FROM Clans WHERE 1 = 0");
    clans.Execute({});
  }  // Release the first result before issuing another statement.
  {
    Statement members(
        connection_.Handle(),
        "SELECT CharacterId, ClanId, MembershipStatus, OfficerSlot FROM ClanMembers WHERE 1 = 0");
    members.Execute({});
  }
  {
    Statement receipts(
        connection_.Handle(),
        "SELECT ClanId, CharacterId, ItemId, InventoryRevision, CreatedAtUnixSeconds FROM ClanCreationReceipts WHERE 1 = 0");
    receipts.Execute({});
  }
  {
    Statement migration(
        connection_.Handle(),
        "SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS WHERE CONSTRAINT_SCHEMA = DATABASE() AND TABLE_NAME = 'Clans' AND CONSTRAINT_NAME = 'CK_Clans_RecruitmentMode' AND CONSTRAINT_TYPE = 'CHECK' AND ENFORCED = 'YES'");
    migration.Execute({});
    std::uint64_t count = 0;
    std::array result{Number(count)};
    if (!migration.Fetch(result) || count != 1)
      throw std::runtime_error("Clan recruitment migration is required.");
  }
  {
    Statement notices(
        connection_.Handle(),
        "SELECT CharacterId, ClanId, NameUtf16, DisbandedAtUnixSeconds FROM ClanDisbandNotices WHERE 1 = 0");
    notices.Execute({});
  }
  {
    Statement history(
        connection_.Handle(),
        "SELECT CharacterId, LastChangedAtUnixSeconds, ExitStatus, Graduated FROM TrainingClanHistory WHERE 1 = 0");
    history.Execute({});
  }
  {
    Statement migration(
        connection_.Handle(),
        "SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS WHERE CONSTRAINT_SCHEMA = DATABASE() AND TABLE_NAME = 'ClanMembers' AND CONSTRAINT_NAME = 'CK_ClanMembers_TrainingRole' AND CONSTRAINT_TYPE = 'CHECK' AND ENFORCED = 'YES'");
    migration.Execute({});
    std::uint64_t count = 0;
    std::array fields{Number(count)};
    if (!migration.Fetch(fields) || count != 1)
      throw std::runtime_error("Training clan migration is required.");
  }
}

std::optional<ClanSnapshot> MySqlClanStore::Load(const ClanIdentity& identity) {
  Transaction transaction(connection_.Handle());
  auto snapshot = LoadInTransaction(identity);
  transaction.Commit();
  return snapshot;
}

std::optional<ClanSnapshot> MySqlClanStore::LoadInTransaction(
    const ClanIdentity& identity) {
  if (!identity.account_id || !identity.character_id) return {};
  auto* connection = connection_.Handle();
  {
    Statement owner(
        connection,
        "SELECT c.CharacterId, a.IsDisabled FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId WHERE c.CharacterId = ? AND c.AccountId = ? FOR SHARE");
    std::array params{Number(identity.character_id),
                      Number(identity.account_id)};
    owner.Execute(params);
    std::uint64_t character = 0;
    std::uint8_t disabled = 0;
    std::array results{Number(character), Number(disabled)};
    if (!owner.Fetch(results) || disabled) return {};
  }
  ClanSnapshot snapshot;
  snapshot.profile.disband_date_bits = DateBits(kDateSentinel);
  std::array<std::uint8_t, 24> name{};
  std::array<std::uint8_t, 192> introduction{};
  unsigned long name_size = 0, introduction_size = 0;
  std::uint32_t capacity = 0, mark = 0, level = 0, points = 0;
  std::uint8_t recruiting = 0;
  std::uint64_t created = 0, disband = 0;
  {
    Statement membership(
        connection,
        "SELECT m.ClanId, m.MembershipStatus, c.NameUtf16, c.IntroductionUtf16, c.MemberCapacity, c.MarkId, c.ClanLevel, c.ClanPoints, c.IsRecruiting, c.CreatedAtUnixSeconds, c.DisbandAtUnixSeconds FROM ClanMembers m JOIN Clans c ON c.ClanId = m.ClanId WHERE m.CharacterId = ?");
    std::array params{Number(identity.character_id)};
    membership.Execute(params);
    std::array results{
        Number(snapshot.profile.clan_id),
        Number(snapshot.profile.membership_status),
        Bytes(name.data(), static_cast<unsigned long>(name.size())),
        Bytes(introduction.data(),
              static_cast<unsigned long>(introduction.size())),
        Number(capacity),
        Number(mark),
        Number(level),
        Number(points),
        Number(recruiting),
        Number(created),
        Number(disband)};
    results[2].length = &name_size;
    results[3].length = &introduction_size;
    if (!membership.Fetch(results)) {
      // Release membership result before reading a possible durable notice.
      snapshot.profile.membership_status = 0;
    }
  }
  if (!snapshot.profile.clan_id) {
    {
      Statement history(
          connection,
          "SELECT ExitStatus, LastChangedAtUnixSeconds FROM TrainingClanHistory WHERE CharacterId = ? AND ExitStatus <> 0");
      std::array params{Number(identity.character_id)};
      history.Execute(params);
      protocol::ClanProfileResponse removed;
      std::array fields{Number(removed.membership_status),
                        Number(snapshot.training_exit_generation)};
      if (history.Fetch(fields)) {
        if (removed.membership_status != 0xffffffecU &&
            removed.membership_status != 0xffffffc4U)
          throw std::runtime_error("Invalid training clan state.");
        removed.clan_id = 99;
        removed.clan_name = u"훈련소";
        removed.disband_date_bits = DateBits(kDateSentinel);
        snapshot.training_exit_notice = std::move(removed);
      }
    }
    Statement notice(
        connection,
        "SELECT ClanId, NameUtf16 FROM ClanDisbandNotices WHERE CharacterId = ?");
    std::array params{Number(identity.character_id)};
    notice.Execute(params);
    protocol::ClanProfileResponse removed;
    std::array results{
        Number(removed.clan_id),
        Bytes(name.data(), static_cast<unsigned long>(name.size()))};
    results[1].length = &name_size;
    if (notice.Fetch(results)) {
      if (!removed.clan_id || removed.clan_id == 99 || name_size < 8 ||
          name_size > name.size())
        throw std::runtime_error("Invalid clan disband notice.");
      removed.clan_name = Text(std::span(name).first(name_size));
      removed.membership_status = 0xffffffd8U;  // Native -40 clears cached ID.
      removed.disband_date_bits = DateBits(kDateSentinel);
      removed.Encode();
      snapshot.disband_notice = std::move(removed);
    }
    return snapshot;
  }
  const auto status = snapshot.profile.membership_status;
  const bool training = snapshot.profile.clan_id == 99;
  if (!snapshot.profile.clan_id || (training && status != 100) ||
      (status != 1 && status != 100 && status != 101 && status != 102) ||
      name_size < (training ? 6U : 8U) || name_size > name.size() ||
      introduction_size < 2 || introduction_size > introduction.size() ||
      !capacity || capacity > 65535 || !level || recruiting > 2 ||
      created >= kDateSentinel ||
      (disband && (disband < created || disband >= kDateSentinel)))
    throw std::runtime_error("Invalid stored clan profile.");
  snapshot.profile.clan_name = Text(std::span(name).first(name_size));
  snapshot.profile.disband_date_bits =
      DateBits(disband ? disband : kDateSentinel);
  if (status == 1) {
    snapshot.profile.Encode();
    return snapshot;
  }
  snapshot.profile.details_available = 1;
  auto& details = snapshot.details.emplace();
  details.clan_name = snapshot.profile.clan_name;
  details.value_at132 = level;
  details.value_at136 = points;
  details.date_at144_bits = DateBits(created);
  details.recruiting = recruiting;
  details.value_at168 = capacity;
  details.text_at200 = Text(std::span(introduction).first(introduction_size));
  details.mark_id = mark;
  if (training) {
    if (snapshot.profile.clan_name != u"훈련소" || mark != 49 || level != 1 ||
        points || recruiting != 2 || capacity != 65535 || created || disband)
      throw std::runtime_error("Invalid training clan state.");
    Statement count(
        connection,
        "SELECT COUNT(*), COALESCE(SUM(MembershipStatus <> 100), 0) FROM ClanMembers WHERE ClanId = 99");
    count.Execute({});
    std::uint64_t members = 0, officers = 0;
    std::array fields{Number(members), Number(officers)};
    if (!count.Fetch(fields) || !members || members > capacity || officers)
      throw std::runtime_error("Invalid training clan state.");
    details.value_at164 = static_cast<std::uint32_t>(members);
    snapshot.profile.Encode();
    details.Encode();
    return snapshot;
  }
  // Scalar 124/128/172, qword 152 and text 176 remain explicit project zero/
  // empty baselines. Names, level, points, count/capacity, intro and birth date
  // are bound by native UI controls, not guesses from the old server schema.
  {
    Statement members(
        connection,
        "SELECT m.MembershipStatus, c.NameUtf16 FROM ClanMembers m JOIN Characters c ON c.CharacterId = m.CharacterId WHERE m.ClanId = ? AND m.MembershipStatus IN (100, 101, 102) ORDER BY m.CharacterId");
    std::array params{Number(snapshot.profile.clan_id)};
    members.Execute(params);
    std::uint32_t role = 0;
    std::array<std::uint8_t, 32> member_name{};
    unsigned long length = 0;
    std::array results{Number(role),
                       Bytes(member_name.data(),
                             static_cast<unsigned long>(member_name.size()))};
    results[1].length = &length;
    bool master = false, deputy = false;
    while (members.Fetch(results)) {
      if (++details.value_at164 > capacity || !length ||
          length > member_name.size())
        throw std::runtime_error("Invalid stored clan roster.");
      const auto text = Text(std::span(member_name).first(length));
      if (features::characters::CharacterCreation::ValidateName(text))
        throw std::runtime_error("Invalid stored clan roster.");
      if (role == 101) {
        if (master) throw std::runtime_error("Invalid stored clan roster.");
        master = true;
        details.text_at76 = text;
      } else if (role == 102) {
        if (deputy) throw std::runtime_error("Invalid stored clan roster.");
        deputy = true;
        details.text_at100 = text;
      }
    }
    if (!master) throw std::runtime_error("Invalid stored clan roster.");
  }
  snapshot.profile.Encode();
  details.Encode();
  return snapshot;
}

std::optional<protocol::ClanDirectoryResponse> MySqlClanStore::Search(
    const ClanIdentity& identity,
    const protocol::ClanDirectoryRequest& request) {
  if (!features::clans::IsClanDirectoryQuerySupported(request))
    throw std::invalid_argument("Unsupported clan directory query.");
  if (!identity.account_id || !identity.character_id) return {};
  auto* connection = connection_.Handle();
  Transaction transaction(connection);
  {
    Statement owner(
        connection,
        "SELECT c.CharacterId, a.IsDisabled FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId WHERE c.CharacterId = ? AND c.AccountId = ? FOR SHARE");
    std::array params{Number(identity.character_id),
                      Number(identity.account_id)};
    owner.Execute(params);
    std::uint64_t character = 0;
    std::uint8_t disabled = 0;
    std::array results{Number(character), Number(disabled)};
    if (!owner.Fetch(results) || disabled) return {};
  }
  protocol::ClanDirectoryResponse response;
  response.page = request.page;
  {
    // Matching and tie-breaks are explicit project policy. Both query fragments
    // are constants; no text from the client is inserted into SQL.
    std::string sql =
        "SELECT c.ClanId, c.NameUtf16, c.IntroductionUtf16, c.MemberCapacity, c.MarkId, c.ClanLevel, c.ClanPoints, c.IsRecruiting, c.CreatedAtUnixSeconds, c.DisbandAtUnixSeconds, COALESCE(master.NameUtf16, ''), (SELECT COUNT(*) FROM ClanMembers n WHERE n.ClanId = c.ClanId AND n.MembershipStatus IN (100, 101, 102)) FROM Clans c LEFT JOIN ClanMembers m ON m.ClanId = c.ClanId AND m.MembershipStatus = 101 LEFT JOIN Characters master ON master.CharacterId = m.CharacterId WHERE c.ClanId <> 99 AND (? = 0 OR c.NameUtf16 = ?) AND (? = 0 OR c.IsRecruiting <> 0)";
    sql +=
        request.query_mode == 5 || request.query_mode == 8
            ? " ORDER BY c.ClanPoints DESC, c.ClanId ASC LIMIT 5 OFFSET ?"
            : " ORDER BY c.CreatedAtUnixSeconds DESC, c.ClanId DESC LIMIT 5 OFFSET ?";
    Statement query(connection, sql);
    const std::uint32_t by_name = request.query_mode == 4 ? 1U : 0U;
    const std::uint32_t recruiting_only =
        request.query_mode == 7 || request.query_mode == 8 ? 1U : 0U;
    const std::uint64_t offset = (std::uint64_t{request.page} - 1) * 5;
    std::array<std::uint8_t, 24> search{};
    for (std::size_t i = 0; i < request.search_text.size(); ++i) {
      search[2 * i] = static_cast<std::uint8_t>(request.search_text[i]);
      search[2 * i + 1] =
          static_cast<std::uint8_t>(request.search_text[i] >> 8);
    }
    std::array params{Number(by_name),
                      Bytes(search.data(), static_cast<unsigned long>(
                                               2 * request.search_text.size())),
                      Number(recruiting_only), Number(offset)};
    query.Execute(params);
    protocol::ClanDirectoryRecord row;
    std::array<std::uint8_t, 24> name{};
    std::array<std::uint8_t, 192> introduction{};
    std::array<std::uint8_t, 32> master{};
    unsigned long name_size = 0, introduction_size = 0, master_size = 0;
    std::uint32_t capacity = 0;
    std::uint64_t created = 0, disband = 0, members = 0;
    std::array results{
        Number(row.clan_id),
        Bytes(name.data(), static_cast<unsigned long>(name.size())),
        Bytes(introduction.data(),
              static_cast<unsigned long>(introduction.size())),
        Number(capacity),
        Number(row.mark_id),
        Number(row.level),
        Number(row.points),
        Number(row.recruitment_mode),
        Number(created),
        Number(disband),
        Bytes(master.data(), static_cast<unsigned long>(master.size())),
        Number(members)};
    results[1].length = &name_size;
    results[2].length = &introduction_size;
    results[10].length = &master_size;
    while (query.Fetch(results)) {
      if (!row.clan_id || row.clan_id == 99 || name_size < 8 ||
          name_size > name.size() || !introduction_size ||
          introduction_size > introduction.size() || !master_size ||
          master_size > master.size() || !capacity || capacity > 65535 ||
          !row.level || row.recruitment_mode > 2 || !members ||
          members > capacity || created >= kDateSentinel ||
          (disband && (disband < created || disband >= kDateSentinel)))
        throw std::runtime_error("Invalid stored clan directory.");
      row.clan_name = Text(std::span(name).first(name_size));
      row.introduction = Text(std::span(introduction).first(introduction_size));
      row.master_name = Text(std::span(master).first(master_size));
      if (features::characters::CharacterCreation::ValidateName(
              row.master_name))
        throw std::runtime_error("Invalid stored clan directory.");
      row.created_at_bits = DateBits(created);
      row.disband_at_bits = DateBits(disband ? disband : kDateSentinel);
      row.member_count = static_cast<std::uint32_t>(members);
      // No trusted login-time field exists yet. Unknown fields and header
      // values remain zero/empty project baselines, not recovered defaults.
      response.clans.push_back(row);
    }
  }  // Drain and release query before committing the read transaction.
  response.Encode();
  transaction.Commit();
  return response;
}
}  // namespace bubble_fighter::server::persistence
