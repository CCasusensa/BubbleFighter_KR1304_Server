#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_STORE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_STORE_H_
#include <stdexcept>
#include <vector>

#include "features/characters/character_store.h"
#include "protocol/social/chat_block_messages.h"
#include "protocol/social/companion_friend_group_messages.h"
#include "protocol/social/companion_mailbox_messages.h"
#include "protocol/social/companion_social_messages.h"
namespace bubble_fighter::server::features::social {
struct SocialSnapshot {
  characters::CharacterRecord owner;
  std::vector<protocol::CompanionSocialRow> rows;
  std::vector<protocol::ChatBlockEntry> blocks;
  std::vector<protocol::CompanionFriendGroup> groups;
  std::vector<protocol::CompanionFriendGroupAssignment> assignments;
};
struct SocialChange {
  protocol::CompanionSocialStatus status =
      protocol::CompanionSocialStatus::kInvalid;
  std::optional<protocol::ChatBlockEntry> target;
};
struct SocialGroupChange {
  protocol::CompanionFriendGroupStatus status =
      protocol::CompanionFriendGroupStatus::kUnavailable;
  SocialSnapshot snapshot;
};
struct SocialMessageChange {
  protocol::CompanionSocialStatus status =
      protocol::CompanionSocialStatus::kUnavailable;
  std::optional<protocol::ChatBlockEntry> target;
  std::u16string sender_name;
  std::uint64_t message_id = 0;
};
struct SocialMailboxSnapshot {
  protocol::CompanionMailboxStatus status =
      protocol::CompanionMailboxStatus::kUnavailable;
  SocialSnapshot snapshot;
  std::vector<protocol::CompanionQueuedMessage> messages;
};
// Explicit companion policy. Stored reads never acknowledge delivery.
inline constexpr std::size_t kMailboxCapacity = 200;
inline constexpr std::uint64_t kMessageLifetimeSeconds = 30 * 24 * 60 * 60;
class SocialStore {
 public:
  virtual ~SocialStore() = default;
  virtual SocialSnapshot Load(const characters::CharacterRecord& owner) = 0;
  virtual std::optional<protocol::ChatBlockEntry> Find(
      std::uint32_t id, std::u16string_view name) = 0;
  virtual SocialChange Change(
      const characters::CharacterRecord& owner,
      const protocol::CompanionSocialRequest& request) = 0;
  // The production store returns the complete snapshot read in the mutation's
  // transaction. Stores without this capability explicitly report unavailable.
  virtual SocialGroupChange ChangeGroup(
      const characters::CharacterRecord& owner,
      const protocol::CompanionFriendGroupRequest&) {
    return {protocol::CompanionFriendGroupStatus::kUnavailable, Load(owner)};
  }
  virtual SocialMessageChange QueueMessage(
      const characters::CharacterRecord&,
      const protocol::CompanionSocialRequest&, std::uint64_t) {
    return {};
  }
  virtual SocialMailboxSnapshot LoadMailbox(
      const characters::CharacterRecord& owner, std::uint64_t) {
    return {protocol::CompanionMailboxStatus::kUnavailable, Load(owner), {}};
  }
  virtual void AcknowledgeMessage(const characters::CharacterRecord&,
                                  std::uint64_t) {
    throw std::runtime_error("Mailbox acknowledgements unavailable.");
  }
  // Checks both block directions and mutual friendship using current durable
  // state.
  virtual protocol::CompanionSocialStatus CanMessage(
      const characters::CharacterRecord& owner, std::uint32_t target) = 0;
};
}  // namespace bubble_fighter::server::features::social

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_STORE_H_
