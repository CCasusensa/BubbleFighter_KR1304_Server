#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MEMBERSHIP_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MEMBERSHIP_MESSAGES_H_

#include <string>

#include "protocol/clans/clan_packet.h"

namespace bubble_fighter::server::protocol {
// Status and result fields preserve uint32 wire bits, including negative native
// codes. Codec acceptance grants no identity, role or transaction authority.
// Manual admission application. Client fields never establish membership.
struct ClanJoinRequest {
  static constexpr std::uint16_t kMessageId = 1032;
  std::uint32_t clan_id = 0;
  std::u16string clan_name;
  ClanPacket Encode() const;
  static ClanJoinRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanJoinRequest&) const = default;
};
// Native automatic/training admission path; kept distinct from manual
// admission.
struct ClanAutoJoinRequest {
  static constexpr std::uint16_t kMessageId = 1033;
  std::uint32_t clan_id = 0;
  std::u16string clan_name;
  std::u16string character_name;
  ClanPacket Encode() const;
  static ClanAutoJoinRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanAutoJoinRequest&) const = default;
};
// Native consumer assigns membership_status even on failure. Use trusted
// current state.
struct ClanJoinResponse {
  static constexpr std::uint16_t kMessageId = 1034;
  std::uint32_t result = 0;
  std::uint32_t membership_status = 0;
  std::uint32_t clan_id = 0;
  std::u16string clan_name;
  ClanPacket Encode() const;
  static ClanJoinResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanJoinResponse&) const = default;
};
// Native notification refreshes the pending application list.
struct ClanApplicationChangedNotification {
  static constexpr std::uint16_t kMessageId = 1035;
  ClanPacket Encode() const;
  static ClanApplicationChangedNotification Decode(const ClanPacket& packet);
  bool operator==(const ClanApplicationChangedNotification&) const = default;
};
// Ordinary leave request; training-specific preceding C1108 is separate.
struct ClanLeaveRequest {
  static constexpr std::uint16_t kMessageId = 1036;
  ClanPacket Encode() const;
  static ClanLeaveRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanLeaveRequest&) const = default;
};
// Native 52E640 reads OLD then NEW status, unlike S1041.
struct ClanLeaveNotification {
  static constexpr std::uint16_t kMessageId = 1037;
  std::u16string clan_name;
  std::uint32_t character_id = 0;
  std::u16string character_name;
  std::uint32_t old_status = 0;
  std::uint32_t new_status = 0;
  ClanPacket Encode() const;
  static ClanLeaveNotification Decode(const ClanPacket& packet);
  bool operator==(const ClanLeaveNotification&) const = default;
};
// Requested status and identity require independent server authorization.
struct ClanMemberChangeRequest {
  static constexpr std::uint16_t kMessageId = 1039;
  std::uint32_t character_id = 0;
  std::u16string character_name;
  std::uint32_t old_status = 0;
  std::uint32_t new_status = 0;
  ClanPacket Encode() const;
  static ClanMemberChangeRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanMemberChangeRequest&) const = default;
};
// Native 52D090 reads NEW then OLD status.
struct ClanMemberChangeResponse {
  static constexpr std::uint16_t kMessageId = 1040;
  std::uint32_t result = 0;
  std::u16string character_name;
  std::uint32_t new_status = 0;
  std::uint32_t old_status = 0;
  ClanPacket Encode() const;
  static ClanMemberChangeResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanMemberChangeResponse&) const = default;
};
// Native 52CE70 reads NEW then OLD status, unlike S1037.
struct ClanMemberChangedNotification {
  static constexpr std::uint16_t kMessageId = 1041;
  std::u16string clan_name;
  std::uint32_t character_id = 0;
  std::u16string character_name;
  std::uint32_t new_status = 0;
  std::uint32_t old_status = 0;
  ClanPacket Encode() const;
  static ClanMemberChangedNotification Decode(const ClanPacket& packet);
  bool operator==(const ClanMemberChangedNotification&) const = default;
};
// Cancel an existing pending application; clan_id is not authority.
struct ClanCancelJoinRequest {
  static constexpr std::uint16_t kMessageId = 1063;
  std::uint32_t clan_id = 0;
  ClanPacket Encode() const;
  static ClanCancelJoinRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanCancelJoinRequest&) const = default;
};
// Native dispatcher skips the second DWORD; its meaning remains unknown.
struct ClanCancelJoinResponse {
  static constexpr std::uint16_t kMessageId = 1064;
  std::uint32_t result = 0;
  std::uint32_t value_at4 = 0;
  ClanPacket Encode() const;
  static ClanCancelJoinResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanCancelJoinResponse&) const = default;
};
// Second DWORD is skipped; consumer receives fixed new=-50, old=1.
struct ClanJoinCancelledNotification {
  static constexpr std::uint16_t kMessageId = 1065;
  std::uint32_t result = 0;
  std::uint32_t value_at4 = 0;
  std::u16string character_name;
  ClanPacket Encode() const;
  static ClanJoinCancelledNotification Decode(const ClanPacket& packet);
  bool operator==(const ClanJoinCancelledNotification&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MEMBERSHIP_MESSAGES_H_
