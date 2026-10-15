#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_CHAT_BLOCK_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_CHAT_BLOCK_MESSAGES_H_
#include <optional>
#include <string>
#include <vector>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
struct ChatBlockEntry {
  std::uint32_t character_id = 0;
  std::u16string name;
  bool operator==(const ChatBlockEntry&) const = default;
};
// BFCLIENT02 EE7660 / EE7420: 3A + id + name, or BA + name.
struct ChatBlockRequest {
  bool remove = false;
  std::optional<std::uint32_t> character_id;
  std::u16string name;
  TcpPacket Encode() const;
  static ChatBlockRequest Decode(const TcpPacket& packet);
};
struct ChatBlockListNotification {
  static constexpr std::uint16_t kMessageId = 170;
  std::vector<ChatBlockEntry> entries;
  TcpPacket Encode() const;
  static ChatBlockListNotification Decode(const TcpPacket& packet);
};
struct ChatBlockAddedNotification {
  static constexpr std::uint16_t kMessageId = 171;
  // A missing identity is the original failure branch (BA + name).
  std::optional<std::uint32_t> character_id;
  std::u16string name;
  TcpPacket Encode() const;
  static ChatBlockAddedNotification Decode(const TcpPacket& packet);
};
struct ChatBlockRemovedNotification {
  static constexpr std::uint16_t kMessageId = 172;
  ChatBlockEntry entry;
  TcpPacket Encode() const;
  static ChatBlockRemovedNotification Decode(const TcpPacket& packet);
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_CHAT_BLOCK_MESSAGES_H_
