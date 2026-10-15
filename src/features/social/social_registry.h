#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_REGISTRY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_REGISTRY_H_
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "features/rooms/room_packet_queue.h"
#include "features/social/social_store.h"
namespace bubble_fighter::server::features::social {
class SocialRegistry;
class SocialLease {
 public:
  SocialLease() = default;
  SocialLease(SocialLease&&) noexcept;
  SocialLease& operator=(SocialLease&&) noexcept;
  SocialLease(const SocialLease&) = delete;
  SocialLease& operator=(const SocialLease&) = delete;
  ~SocialLease();
  explicit operator bool() const noexcept { return registry_ != nullptr; }
  void Release() noexcept;
  void Process(const protocol::CompanionSocialRequest& request);
  void ProcessGroups(const protocol::CompanionFriendGroupRequest& request);
  void ProcessMailbox(const protocol::CompanionMailboxRequest& request);
  void Block(const protocol::ChatBlockRequest& request);
  void RefreshName();

 private:
  friend class SocialRegistry;
  std::shared_ptr<SocialRegistry> registry_;
  std::uint64_t id_ = 0;
  std::uint64_t token_ = 0;
};
class SocialRegistry : public std::enable_shared_from_this<SocialRegistry> {
 public:
  using StoreFactory = std::function<std::unique_ptr<SocialStore>()>;
  using Clock = std::function<std::uint64_t()>;
  explicit SocialRegistry(StoreFactory factory, Clock clock = {});
  SocialLease Enter(const characters::CharacterRecord& owner,
                    rooms::RoomPacketSink sink);
  bool AllowsPublicChat(std::uint64_t sender, std::uint64_t recipient) const;

 private:
  friend class SocialLease;
  struct Member {
    characters::CharacterRecord character;
    rooms::RoomPacketSink sink;
    SocialSnapshot snapshot;
    std::uint64_t token = 0;
    bool companion = false;
    bool group_companion = false;
    bool mailbox_companion = false;
    std::set<std::uint64_t> mailbox_inflight;
    std::deque<std::uint64_t> mailbox_acknowledged;
  };
  void Leave(std::uint64_t id, std::uint64_t token) noexcept;
  void Process(std::uint64_t id, std::uint64_t token,
               const protocol::CompanionSocialRequest& request);
  void ProcessGroups(std::uint64_t id, std::uint64_t token,
                     const protocol::CompanionFriendGroupRequest& request);
  void ProcessMailbox(std::uint64_t id, std::uint64_t token,
                      const protocol::CompanionMailboxRequest& request);
  void OfferMailbox(Member& member, SocialStore& store);
  void SendQueued(Member& member,
                  const protocol::CompanionSocialRequest& request);
  static void RememberAcknowledgement(Member& member, std::uint64_t id);
  void Block(std::uint64_t id, std::uint64_t token,
             const protocol::ChatBlockRequest& request);
  void RefreshName(std::uint64_t id, std::uint64_t token);
  Member& Require(std::uint64_t id, std::uint64_t token);
  void Refresh(Member& member, SocialStore& store);
  void UseSnapshot(Member& member, SocialSnapshot snapshot);
  protocol::CompanionSocialRosterNotification Roster(
      const Member& member, protocol::CompanionSocialStatus status,
      protocol::CompanionSocialAction action);
  void Publish(Member& member,
               protocol::CompanionSocialStatus status =
                   protocol::CompanionSocialStatus::kOk,
               protocol::CompanionSocialAction action =
                   protocol::CompanionSocialAction::kList);
  void RefreshPeers(std::uint64_t id, SocialStore& store);
  static void Deliver(Member& member,
                      std::vector<protocol::TcpPacket> packets) noexcept;
  StoreFactory factory_;
  Clock clock_;
  std::mutex mutex_;
  std::map<std::uint64_t, Member> members_;
  std::uint64_t next_token_ = 1;
  // Only this small cache lock is acquired while a room/plaza is routing.
  mutable std::mutex block_mutex_;
  std::map<std::uint64_t, std::set<std::uint64_t>> blocks_;
};
}  // namespace bubble_fighter::server::features::social

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_REGISTRY_H_
