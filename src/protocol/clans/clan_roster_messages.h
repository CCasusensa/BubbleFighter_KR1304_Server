#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_ROSTER_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_ROSTER_MESSAGES_H_

#include <string>
#include <vector>

#include "protocol/clans/clan_packet.h"

namespace bubble_fighter::server::protocol {
// at* names denote native row memory offsets, not fixed payload offsets.
// Preserve opaque integers and QWORD bits without inferring stats/date meaning.
struct ClanMemberRecord {
  std::uint32_t character_id = 0;
  std::u16string character_name;
  std::uint32_t value_at36 = 0;
  std::uint32_t value_at28 = 0;
  std::uint32_t membership_status = 0;
  bool operator==(const ClanMemberRecord&) const = default;
};
struct ClanMemberDetailsRecord {
  std::uint32_t character_id = 0;
  std::u16string character_name;
  std::uint32_t value_at28 = 0;
  std::uint32_t membership_status = 0;
  std::uint32_t value_at40 = 0;
  std::uint32_t value_at44 = 0;
  std::uint64_t value_at48_bits = 0;
  std::uint64_t value_at56_bits = 0;
  std::uint64_t value_at64_bits = 0;
  std::uint32_t value_at72 = 0;
  std::uint32_t value_at76 = 0;
  std::uint32_t value_at80 = 0;
  std::uint32_t value_at84 = 0;
  bool operator==(const ClanMemberDetailsRecord&) const = default;
};
struct ClanApplicantRecord {
  std::uint32_t character_id = 0;
  std::u16string character_name;
  std::uint32_t value_at28 = 0;
  std::uint32_t membership_status = 0;
  std::uint64_t value_at56_bits = 0;
  std::uint32_t value_at40 = 0;
  std::uint32_t value_at44 = 0;
  std::uint32_t value_at48 = 0;
  std::uint32_t value_at52 = 0;
  bool operator==(const ClanApplicantRecord&) const = default;
};
struct ClanRosterRequest {
  static constexpr std::uint16_t kMessageId = 1011;
  ClanPacket Encode() const;
  static ClanRosterRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanRosterRequest&) const = default;
};
struct ClanRosterResponse {
  static constexpr std::uint16_t kMessageId = 1012;
  std::vector<ClanMemberRecord> members;
  ClanPacket Encode() const;
  static ClanRosterResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanRosterResponse&) const = default;
};
// The original query writer emits value_at0=1; other wire bits are preserved.
struct ClanMemberDetailsRequest {
  static constexpr std::uint16_t kMessageId = 1013;
  std::uint32_t value_at0 = 1;
  ClanPacket Encode() const;
  static ClanMemberDetailsRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanMemberDetailsRequest&) const = default;
};
// The first DWORD is skipped by the native parser. It is not a proven result.
struct ClanMemberDetailsResponse {
  static constexpr std::uint16_t kMessageId = 1014;
  std::uint32_t value_at0 = 0;
  std::vector<ClanMemberDetailsRecord> members;
  ClanPacket Encode() const;
  static ClanMemberDetailsResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanMemberDetailsResponse&) const = default;
};
struct ClanApplicantsRequest {
  static constexpr std::uint16_t kMessageId = 1069;
  ClanPacket Encode() const;
  static ClanApplicantsRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanApplicantsRequest&) const = default;
};
struct ClanApplicantsResponse {
  static constexpr std::uint16_t kMessageId = 1070;
  std::vector<ClanApplicantRecord> applicants;
  ClanPacket Encode() const;
  static ClanApplicantsResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanApplicantsResponse&) const = default;
};
// UI emits 0 closed, 1 manual, 2 automatic. Wire data grants no authority.
struct ClanRecruitmentRequest {
  static constexpr std::uint16_t kMessageId = 1102;
  std::uint32_t clan_id = 0;
  std::uint32_t recruitment_mode = 0;
  ClanPacket Encode() const;
  static ClanRecruitmentRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanRecruitmentRequest&) const = default;
};
// Native skips value_at0 and updates mode on any nonzero result.
struct ClanRecruitmentResponse {
  static constexpr std::uint16_t kMessageId = 1103;
  std::uint32_t value_at0 = 0;
  std::uint32_t result = 0;
  std::uint32_t recruitment_mode = 0;
  ClanPacket Encode() const;
  static ClanRecruitmentResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanRecruitmentResponse&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_ROSTER_MESSAGES_H_
