#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COMPANION_FRIEND_GROUP_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COMPANION_FRIEND_GROUP_MESSAGES_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// Project companion extension, not the native Messenger SDK wire protocol.
// Groups are private to the authenticated owner. Zero denotes ungrouped.
enum class CompanionFriendGroupAction : std::uint8_t {
  kList,
  kCreate,
  kRename,
  kRemove,
  kMove
};
enum class CompanionFriendGroupStatus : std::uint32_t {
  kOk,
  kNotFound,
  kDuplicate,
  kCapacity,
  kNotFriend,
  kNotEmpty,
  kStale,
  kInvalid,
  kUnavailable
};
struct CompanionFriendGroupRequest {
  static constexpr std::uint16_t kMessageId = 65012;
  CompanionFriendGroupAction action = CompanionFriendGroupAction::kList;
  std::uint32_t group_id = 0;
  std::uint32_t friend_id = 0;
  // A move is conditional on the current owner's assignment matching this ID.
  std::uint32_t expected_group_id = 0;
  std::u16string name;
  TcpPacket Encode() const;
  static CompanionFriendGroupRequest Decode(const TcpPacket& packet);
};
struct CompanionFriendGroup {
  std::uint32_t group_id = 0;
  std::u16string name;
  bool operator==(const CompanionFriendGroup&) const = default;
};
struct CompanionFriendGroupAssignment {
  std::uint32_t friend_id = 0;
  std::uint32_t group_id = 0;
  bool operator==(const CompanionFriendGroupAssignment&) const = default;
};
struct CompanionFriendGroupsNotification {
  static constexpr std::uint16_t kMessageId = 65013;
  static constexpr std::size_t kMaximumGroups = 32;
  static constexpr std::size_t kMaximumAssignments = 200;
  CompanionFriendGroupStatus status = CompanionFriendGroupStatus::kOk;
  CompanionFriendGroupAction action = CompanionFriendGroupAction::kList;
  std::vector<CompanionFriendGroup> groups;
  // Ungrouped friends are absent; each friend has at most one assignment.
  std::vector<CompanionFriendGroupAssignment> assignments;
  TcpPacket Encode() const;
  static CompanionFriendGroupsNotification Decode(const TcpPacket& packet);
};
// Native name buffers contain 32 UTF-16 units including their terminator.
// Control characters, invalid surrogates and blank names are project policy.
bool ValidCompanionFriendGroupName(std::u16string_view name);
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COMPANION_FRIEND_GROUP_MESSAGES_H_
