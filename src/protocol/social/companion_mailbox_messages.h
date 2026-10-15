#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COMPANION_MAILBOX_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COMPANION_MAILBOX_MESSAGES_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// Project durable chat extension. Native Note events have empty game handlers.
// Acknowledgement means adapter handoff, never human read.
enum class CompanionMailboxAction : std::uint8_t { kFetch, kAcknowledge };
enum class CompanionMailboxStatus : std::uint32_t {
  kOk,
  kUnavailable,
  kRateLimited
};
struct CompanionMailboxRequest {
  static constexpr std::uint16_t kMessageId = 65016;
  CompanionMailboxAction action = CompanionMailboxAction::kFetch;
  std::uint64_t message_id = 0;
  TcpPacket Encode() const;
  static CompanionMailboxRequest Decode(const TcpPacket& packet);
};
struct CompanionQueuedMessage {
  std::uint64_t message_id = 0;
  std::uint32_t sender_id = 0;
  std::uint64_t sent_at_unix_seconds = 0;
  std::u16string sender_name;
  std::u16string text;
  bool operator==(const CompanionQueuedMessage&) const = default;
};
struct CompanionMailboxNotification {
  static constexpr std::uint16_t kMessageId = 65017;
  static constexpr std::size_t kMaximumMessages = 20;
  static constexpr std::uint64_t kMaximumTimestamp = 4102444800;
  CompanionMailboxStatus status = CompanionMailboxStatus::kOk;
  std::vector<CompanionQueuedMessage> messages;
  TcpPacket Encode() const;
  static CompanionMailboxNotification Decode(const TcpPacket& packet);
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COMPANION_MAILBOX_MESSAGES_H_
