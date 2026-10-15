#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_HANDOFF_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_HANDOFF_MESSAGES_H_
#include <array>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// Project companion v1. These messages are NOT recovered native protocol.
// Secrets must never be logged. Native C54 remains exactly eight bytes.
using HandoffSecret = std::array<std::uint8_t, 32>;
struct HandoffCapabilityRequest {
  static constexpr std::uint16_t kMessageId = 65020;
  TcpPacket Encode() const;
  static HandoffCapabilityRequest Decode(const TcpPacket& packet);
};
struct HandoffGrantNotification {
  static constexpr std::uint16_t kMessageId = 65021;
  std::uint32_t character_id = 0;
  std::uint32_t channel_category = 0;
  HandoffSecret secret{};
  TcpPacket Encode() const;
  static HandoffGrantNotification Decode(const TcpPacket& packet);
};
struct HandoffProofRequest {
  static constexpr std::uint16_t kMessageId = 65022;
  std::uint32_t character_id = 0;
  HandoffSecret secret{};
  TcpPacket Encode() const;
  static HandoffProofRequest Decode(const TcpPacket& packet);
};
struct HandoffCancelledNotification {
  static constexpr std::uint16_t kMessageId = 65023;
  TcpPacket Encode() const;
  static HandoffCancelledNotification Decode(const TcpPacket& packet);
};
}  // namespace bubble_fighter::server::protocol
#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_HANDOFF_MESSAGES_H_
