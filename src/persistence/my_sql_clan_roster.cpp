#include <array>
#include <limits>

#include "features/characters/character_creation.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"
#include "protocol/codec/packet_reader.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
using features::clans::ClanRosterKind;

std::optional<std::vector<protocol::ClanPacket>> MySqlClanStore::ReadRoster(
    const features::clans::ClanIdentity& identity, ClanRosterKind kind) {
  {
    Statement isolation(connection_.Handle(),
                        "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    isolation.Execute({});
  }
  Transaction transaction(connection_.Handle());
  auto result = ReadRosterInTransaction(identity, kind);
  transaction.Commit();
  return result;
}

std::optional<std::vector<protocol::ClanPacket>>
MySqlClanStore::ReadRosterInTransaction(
    const features::clans::ClanIdentity& identity, ClanRosterKind kind) {
  using namespace protocol;
  std::size_t minimum_row = 0;
  std::size_t header = 4;
  switch (kind) {
    case ClanRosterKind::kMembers:
      minimum_row = 18;
      break;
    case ClanRosterKind::kDetails:
      minimum_row = 62;
      header = 8;
      break;
    case ClanRosterKind::kApplicants:
      minimum_row = 38;
      break;
    default:
      throw std::invalid_argument("Unsupported clan roster query.");
  }
  auto* connection = connection_.Handle();
  // All nonlocking member reads share the transaction's consistent snapshot;
  // LoadInTransaction also holds the trusted account/character FOR SHARE.
  const auto snapshot = LoadInTransaction(identity);
  if (!snapshot) return {};
  const auto role = snapshot->profile.membership_status;
  if (snapshot->profile.clan_id == 99 || !snapshot->details ||
      (kind == ClanRosterKind::kApplicants && role != 101 && role != 102)) {
    std::vector<ClanPacket> result{snapshot->profile.Encode()};
    return result;
  }
  ClanRosterResponse members;
  ClanMemberDetailsResponse details;
  ClanApplicantsResponse applicants;
  std::size_t count = 0;
  std::size_t remaining = ClanPacketCodec::kMaxPayloadSize - header;
  {
    Statement query(
        connection,
        "SELECT m.CharacterId, c.NameUtf16, m.MembershipStatus FROM ClanMembers m JOIN Characters c ON c.CharacterId = m.CharacterId WHERE m.ClanId = ? AND ((? = 1 AND m.MembershipStatus = 1) OR (? = 0 AND m.MembershipStatus IN (100, 101, 102))) ORDER BY m.CharacterId LIMIT ?");
    const std::uint32_t pending = kind == ClanRosterKind::kApplicants ? 1U : 0U;
    // Fetch at most one over the minimum-row bound. Do not truncate a roster.
    const std::uint64_t limit = remaining / minimum_row + 1;
    std::array params{Number(snapshot->profile.clan_id), Number(pending),
                      Number(pending), Number(limit)};
    query.Execute(params);
    std::uint64_t character = 0;
    std::uint32_t status = 0;
    std::array<std::uint8_t, 32> bytes{};
    unsigned long length = 0;
    std::array results{
        Number(character),
        Bytes(bytes.data(), static_cast<unsigned long>(bytes.size())),
        Number(status)};
    results[1].length = &length;
    while (query.Fetch(results)) {
      if (!character ||
          character > (std::numeric_limits<std::uint32_t>::max)() || !length ||
          length > bytes.size() || length % 2 ||
          (pending ? status != 1
                   : (status != 100 && status != 101 && status != 102)))
        throw std::runtime_error("Invalid stored clan roster.");
      PacketReader reader(std::span(bytes).first(length));
      std::u16string name;
      while (reader.RemainingBytes()) name.push_back(reader.ReadUInt16());
      if (features::characters::CharacterCreation::ValidateName(name))
        throw std::runtime_error("Invalid stored clan roster.");
      const std::size_t charge = minimum_row + length;
      if (charge > remaining)
        throw std::runtime_error("Clan roster exceeds native frame capacity.");
      remaining -= charge;
      ++count;
      const auto id = static_cast<std::uint32_t>(character);
      // Native scene 0 is "unknown"; no presence service is installed here.
      // Stats/date fields are explicit zero baselines, not trusted
      // observations.
      if (kind == ClanRosterKind::kMembers) {
        members.members.push_back({id, std::move(name), 0, 0, status});
      } else if (kind == ClanRosterKind::kDetails) {
        ClanMemberDetailsRecord row;
        row.character_id = id;
        row.character_name = std::move(name);
        row.membership_status = status;
        details.members.push_back(std::move(row));
      } else {
        ClanApplicantRecord row;
        row.character_id = id;
        row.character_name = std::move(name);
        row.membership_status = status;
        applicants.applicants.push_back(std::move(row));
      }
    }
  }
  if (kind != ClanRosterKind::kApplicants &&
      count != snapshot->details->value_at164)
    throw std::runtime_error("Invalid stored clan roster.");
  std::vector<ClanPacket> result;
  if (kind == ClanRosterKind::kMembers) {
    result.push_back(members.Encode());
  } else if (kind == ClanRosterKind::kDetails) {
    result.push_back(details.Encode());
  } else {
    // Native S1070 sends individual C1039 rejections when its cached mode is 0.
    // Keep this cache consistent with the database snapshot of the applicants.
    result.push_back(snapshot->details->Encode());
    result.push_back(applicants.Encode());
  }
  return result;
}
}  // namespace bubble_fighter::server::persistence
