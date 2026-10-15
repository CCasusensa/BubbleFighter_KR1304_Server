#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_VISITATION_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_VISITATION_MESSAGES_H_

#include <string>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// BFCLIENT02 939820: request permission to visit the named player.
struct VisitationRequest {
  static constexpr std::uint16_t kMessageId = 112;
  std::u16string target_name;
  TcpPacket Encode() const;
  static VisitationRequest Decode(const TcpPacket& packet);
};
// 9D77E0: native byte at +4 is zero-extended to a DWORD on the wire.
// Current caller supplies 0/1 for differing refusal paths; policy is unknown.
struct DeclineVisitationRequest {
  static constexpr std::uint16_t kMessageId = 113;
  std::uint32_t visitation_key = 0;
  std::uint32_t value_at4 = 0;
  TcpPacket Encode() const;
  static DeclineVisitationRequest Decode(const TcpPacket& packet);
};
struct AcceptVisitationRequest {
  static constexpr std::uint16_t kMessageId = 114;
  std::uint32_t visitation_key = 0;
  TcpPacket Encode() const;
  static AcceptVisitationRequest Decode(const TcpPacket& packet);
};
struct VisitationFailedNotification {
  static constexpr std::uint16_t kMessageId = 123;
  std::u16string target_name;
  std::uint32_t reason = 0;
  TcpPacket Encode() const;
  static VisitationFailedNotification Decode(const TcpPacket& packet);
};
struct AskedVisitationNotification {
  static constexpr std::uint16_t kMessageId = 124;
  std::uint32_t visitation_key = 0;
  std::u16string visitor_name;
  // 9AB8F0 reads DWORD then tests nonzero. The nonzero path auto-accepts;
  // original server selection policy is unknown. This server sends zero.
  std::uint32_t value_at28 = 0;
  TcpPacket Encode() const;
  static AskedVisitationNotification Decode(const TcpPacket& packet);
};
// 9A9710 copies these fields into C115 and supplies byte_at32 = 0.
struct VisitationAcceptedNotification {
  static constexpr std::uint16_t kMessageId = 125;
  std::u16string target_name;
  std::uint32_t address = 0;
  std::uint16_t port = 0;
  TcpPacket Encode() const;
  static VisitationAcceptedNotification Decode(const TcpPacket& packet);
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_VISITATION_MESSAGES_H_
