#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MY_SQL_SOCIAL_STORE_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MY_SQL_SOCIAL_STORE_H_
#include "features/social/social_store.h"
#include "persistence/my_sql_connection.h"
namespace bubble_fighter::server::persistence {
class MySqlSocialStore final : public features::social::SocialStore {
 public:
  explicit MySqlSocialStore(const std::filesystem::path& configuration);
  features::social::SocialSnapshot Load(
      const features::characters::CharacterRecord& owner) override;
  std::optional<protocol::ChatBlockEntry> Find(
      std::uint32_t id, std::u16string_view name) override;
  features::social::SocialChange Change(
      const features::characters::CharacterRecord& owner,
      const protocol::CompanionSocialRequest& request) override;
  features::social::SocialGroupChange ChangeGroup(
      const features::characters::CharacterRecord& owner,
      const protocol::CompanionFriendGroupRequest& request) override;
  protocol::CompanionSocialStatus CanMessage(
      const features::characters::CharacterRecord& owner,
      std::uint32_t target) override;
  features::social::SocialMessageChange QueueMessage(
      const features::characters::CharacterRecord& owner,
      const protocol::CompanionSocialRequest& request,
      std::uint64_t now) override;
  features::social::SocialMailboxSnapshot LoadMailbox(
      const features::characters::CharacterRecord& owner,
      std::uint64_t now) override;
  void AcknowledgeMessage(const features::characters::CharacterRecord& owner,
                          std::uint64_t message_id) override;

 private:
  features::social::SocialSnapshot LoadLocked(
      const features::characters::CharacterRecord& owner);
  void ReadGroups(features::social::SocialSnapshot& snapshot);
  void LockPair(const features::characters::CharacterRecord& owner,
                std::uint32_t target);
  bool Blocked(std::uint64_t first, std::uint64_t second);
  std::uint64_t RelationshipCount(std::uint64_t id);
  void ExpireMessages(std::uint64_t recipient, std::uint64_t now);
  MySqlConnection connection_;
};
}  // namespace bubble_fighter::server::persistence

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MY_SQL_SOCIAL_STORE_H_
