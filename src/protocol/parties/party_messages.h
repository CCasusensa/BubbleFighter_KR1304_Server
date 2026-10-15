#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_PARTIES_PARTY_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_PARTIES_PARTY_MESSAGES_H_

#include <cstdint>
#include <string>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// BFCLIENT02 party requests and notifications. S201 is the separate full
// membership snapshot. Request/response IDs 302 and 303 are directional.
struct LeavePartyRequest {
  static constexpr std::uint16_t kMessageId = 202;
  TcpPacket Encode() const;
  static LeavePartyRequest Decode(const TcpPacket& packet);
  bool operator==(const LeavePartyRequest&) const = default;
};

struct KickPartyRequest {
  static constexpr std::uint16_t kMessageId = 302;
  std::u16string target_name;
  TcpPacket Encode() const;
  static KickPartyRequest Decode(const TcpPacket& packet);
  bool operator==(const KickPartyRequest&) const = default;
};

struct ChangePartyLeaderRequest {
  static constexpr std::uint16_t kMessageId = 303;
  std::u16string target_name;
  TcpPacket Encode() const;
  static ChangePartyLeaderRequest Decode(const TcpPacket& packet);
  bool operator==(const ChangePartyLeaderRequest&) const = default;
};

struct PartyLeaderLeftNotification {
  static constexpr std::uint16_t kMessageId = 203;
  std::u16string previous_leader_name;
  std::u16string new_leader_name;
  TcpPacket Encode() const;
  static PartyLeaderLeftNotification Decode(const TcpPacket& packet);
  bool operator==(const PartyLeaderLeftNotification&) const = default;
};

struct PartyLeaveNotification {
  static constexpr std::uint16_t kMessageId = 204;
  TcpPacket Encode() const;
  static PartyLeaveNotification Decode(const TcpPacket& packet);
  bool operator==(const PartyLeaveNotification&) const = default;
};

struct PartyMemberLeftNotification {
  static constexpr std::uint16_t kMessageId = 205;
  std::u16string member_name;
  TcpPacket Encode() const;
  static PartyMemberLeftNotification Decode(const TcpPacket& packet);
  bool operator==(const PartyMemberLeftNotification&) const = default;
};

struct PartyMemberJoinedNotification {
  static constexpr std::uint16_t kMessageId = 206;
  std::u16string member_name;
  TcpPacket Encode() const;
  static PartyMemberJoinedNotification Decode(const TcpPacket& packet);
  bool operator==(const PartyMemberJoinedNotification&) const = default;
};

struct KickPartyResponse {
  static constexpr std::uint16_t kMessageId = 302;
  std::uint32_t reason = 0;
  std::u16string target_name;
  TcpPacket Encode() const;
  static KickPartyResponse Decode(const TcpPacket& packet);
  bool operator==(const KickPartyResponse&) const = default;
};

struct ChangePartyLeaderResponse {
  static constexpr std::uint16_t kMessageId = 303;
  std::u16string new_leader_name;
  std::u16string previous_leader_name;
  std::uint32_t reason = 0;
  TcpPacket Encode() const;
  static ChangePartyLeaderResponse Decode(const TcpPacket& packet);
  bool operator==(const ChangePartyLeaderResponse&) const = default;
};

struct KickedFromPartyNotification {
  static constexpr std::uint16_t kMessageId = 304;
  std::u16string kicker_name;
  TcpPacket Encode() const;
  static KickedFromPartyNotification Decode(const TcpPacket& packet);
  bool operator==(const KickedFromPartyNotification&) const = default;
};

struct PartyKickNotification {
  static constexpr std::uint16_t kMessageId = 307;
  std::u16string kicker_name;
  std::u16string target_name;
  TcpPacket Encode() const;
  static PartyKickNotification Decode(const TcpPacket& packet);
  bool operator==(const PartyKickNotification&) const = default;
};

}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_PARTIES_PARTY_MESSAGES_H_
