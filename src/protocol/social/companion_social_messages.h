#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COMPANION_SOCIAL_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COMPANION_SOCIAL_MESSAGES_H_
#include <string>
#include <vector>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// Explicit project companion protocol. These are not recovered Nexon messages.
enum class CompanionSocialAction : std::uint8_t {
  kList,
  kSearch,
  kAdd,
  kAccept,
  kDecline,
  kRemove,
  kBlock,
  kUnblock,
  kMessage
};
enum class CompanionSocialStatus : std::uint32_t {
  kOk,
  kNotFound,
  kSelf,
  kBlocked,
  kCapacity,
  kNoRequest,
  kNotFriend,
  kOffline,
  kInvalid,
  kUnavailable
};
struct CompanionSocialRequest {
  static constexpr std::uint16_t kMessageId = 65010;
  CompanionSocialAction action = CompanionSocialAction::kList;
  std::uint32_t target_id = 0;
  std::u16string target_name;
  std::u16string text;
  TcpPacket Encode() const;
  static CompanionSocialRequest Decode(const TcpPacket& packet);
};
struct CompanionSocialRow {
  std::uint32_t character_id = 0;
  std::u16string name;
  std::uint8_t relationship =
      0;  // 0 search, 1 friend, 2 incoming, 3 outgoing, 4 blocked.
  bool online = false;
  bool operator==(const CompanionSocialRow&) const = default;
};
struct CompanionSocialRosterNotification {
  static constexpr std::uint16_t kMessageId = 65011;
  CompanionSocialStatus status = CompanionSocialStatus::kOk;
  CompanionSocialAction action = CompanionSocialAction::kList;
  std::u16string notice;
  std::vector<CompanionSocialRow> rows;
  TcpPacket Encode() const;
  static CompanionSocialRosterNotification Decode(const TcpPacket& packet);
};
struct CompanionPrivateMessageNotification {
  static constexpr std::uint16_t kMessageId = 65015;
  std::uint32_t peer_id = 0;
  std::u16string peer_name;
  std::u16string speaker;
  std::u16string text;
  TcpPacket Encode() const;
  static CompanionPrivateMessageNotification Decode(const TcpPacket& packet);
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COMPANION_SOCIAL_MESSAGES_H_
