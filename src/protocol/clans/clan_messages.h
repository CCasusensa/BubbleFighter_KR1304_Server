#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MESSAGES_H_

#include <string>

#include "protocol/clans/clan_packet.h"
#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// S21 -> C1005 copies these fields without interpreting the opaque Passport.
// Identifier/text/scalars are wire data, never proof of authenticated identity.
struct ClanCredentials {
  std::uint32_t value_at0 = 0;
  std::u16string text_at4;
  std::uint32_t value_at28 = 0;
  std::uint32_t value_at32 = 0;
  std::vector<std::uint8_t> passport;
  bool operator==(const ClanCredentials&) const = default;
};
struct ClanPassportRequest {
  static constexpr std::uint16_t kMessageId = 25;
  TcpPacket Encode() const;
  static ClanPassportRequest Decode(const TcpPacket& packet);
};
struct ClanPassportResponse {
  static constexpr std::uint16_t kMessageId = 21;
  ClanCredentials credentials;
  TcpPacket Encode() const;
  static ClanPassportResponse Decode(const TcpPacket& packet);
};
struct ClanHelloNotification {
  static constexpr std::uint16_t kMessageId = 1002;
  ClanPacket Encode() const;
  static ClanHelloNotification Decode(const ClanPacket& packet);
};
struct ClanHeartbeatRequest {
  static constexpr std::uint16_t kMessageId = 1003;
  ClanPacket Encode() const;
  static ClanHeartbeatRequest Decode(const ClanPacket& packet);
};
struct ClanLoginRequest {
  static constexpr std::uint16_t kMessageId = 1005;
  ClanCredentials credentials;
  ClanPacket Encode() const;
  static ClanLoginRequest Decode(const ClanPacket& packet);
};
struct ClanLoginResponse {
  static constexpr std::uint16_t kMessageId = 1006;
  // Signed native values are preserved as their uint32 wire bits. Native
  // result 1 requests C1015; -1/-2 invoke a failure handler.
  std::uint32_t result = 0;
  ClanPacket Encode() const;
  static ClanLoginResponse Decode(const ClanPacket& packet);
};
struct ClanProfileRequest {
  static constexpr std::uint16_t kMessageId = 1015;
  ClanPacket Encode() const;
  static ClanProfileRequest Decode(const ClanPacket& packet);
};
struct ClanProfileResponse {
  static constexpr std::uint16_t kMessageId = 1016;
  std::uint8_t details_available = 0;
  // For status 1 this is the requested clan, not accepted membership.
  std::uint32_t clan_id = 0;
  // Native 1 pending, 100 member, 101 master, 102 deputy. Wire values
  // (including signed/unknown bits) do not authorize server operations.
  std::uint32_t membership_status = 0;
  std::u16string clan_name;
  // Native OLE DATE. Preserve the bits; zero is not a proven safe default.
  std::uint64_t disband_date_bits = 0;
  ClanPacket Encode() const;
  static ClanProfileResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanProfileResponse&) const = default;
};
struct ClanDetailsRequest {
  static constexpr std::uint16_t kMessageId = 1017;
  ClanPacket Encode() const;
  static ClanDetailsRequest Decode(const ClanPacket& packet);
};
struct ClanDetailsResponse {
  static constexpr std::uint16_t kMessageId = 1018;
  // at* refers to the native manager's memory offsets, not wire offsets.
  std::u16string clan_name;
  std::uint32_t value_at124 = 0;
  std::u16string text_at76;
  std::uint32_t value_at128 = 0;
  std::u16string text_at100;
  std::uint32_t value_at132 = 0;
  std::uint32_t value_at136 = 0;
  std::uint64_t date_at144_bits = 0;
  std::uint64_t value_at152_bits = 0;
  std::uint32_t recruiting = 0;
  std::uint32_t value_at164 = 0;
  std::uint32_t value_at172 = 0;
  std::u16string text_at176;
  std::u16string text_at200;
  std::uint32_t mark_id = 0;
  std::uint32_t value_at168 = 0;
  ClanPacket Encode() const;
  static ClanDetailsResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanDetailsResponse&) const = default;
};
struct ClanCreateRequest {
  static constexpr std::uint16_t kMessageId = 1027;
  std::u16string name;
  std::uint32_t value_after_name = 0;
  std::u16string introduction;
  // Native UI passes the selected Function=22 item's key and parameter.
  // The server must independently verify catalog and owned inventory.
  std::uint32_t item_id = 0;
  std::uint32_t item_parameter = 0;
  ClanPacket Encode() const;
  static ClanCreateRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanCreateRequest&) const = default;
};
struct ClanCreateResponse {
  static constexpr std::uint16_t kMessageId = 1028;
  // Native UI: 1 success, 2 duplicate name, -100/-101 invalid name/intro
  // length, -102/-103 prohibited text, -104 pending join, -105 in a clan.
  std::uint32_t result = 0;
  // Success: native 517FE0 -> 805C50 sends value_at4 as C806 item_id.
  // value_at8 is forwarded to platform telemetry; its meaning is unproven.
  std::uint32_t value_at4 = 0;
  std::uint32_t value_at8 = 0;
  ClanPacket Encode() const;
  static ClanCreateResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanCreateResponse&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MESSAGES_H_
