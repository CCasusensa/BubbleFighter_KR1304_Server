#include "features/social/social_registry.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

#include "features/chat/scene_chat.h"
namespace bubble_fighter::server::features::social {
using namespace protocol;
namespace {
std::u16string Notice(CompanionSocialStatus status,
                      CompanionSocialAction action) {
  if (status == CompanionSocialStatus::kCapacity &&
      action == CompanionSocialAction::kMessage) {
    return u"对方待接收的消息已满，请稍后重试。";
  }
  switch (status) {
    case CompanionSocialStatus::kNotFound:
      return u"没有找到该角色，昵称可能已经变更。";
    case CompanionSocialStatus::kSelf:
      return u"不能对自己执行此操作。";
    case CompanionSocialStatus::kBlocked:
      return u"当前无法与该角色建立联系。";
    case CompanionSocialStatus::kCapacity:
      return u"好友或屏蔽名单已满。";
    case CompanionSocialStatus::kNoRequest:
      return u"没有可由你确认的好友申请。";
    case CompanionSocialStatus::kNotFriend:
      return u"请先添加对方为好友。";
    case CompanionSocialStatus::kOffline:
      return u"对方当前不在线，消息未发送。";
    case CompanionSocialStatus::kInvalid:
      return u"社交请求无效。";
    case CompanionSocialStatus::kUnavailable:
      return u"社交服务暂时不可用，请稍后重试。";
    default:
      break;
  }
  switch (action) {
    case CompanionSocialAction::kAdd:
      return u"好友申请已处理，请等待对方确认。";
    case CompanionSocialAction::kAccept:
      return u"已成为好友。";
    case CompanionSocialAction::kDecline:
      return u"已拒绝好友申请。";
    case CompanionSocialAction::kRemove:
      return u"已删除好友或取消申请。";
    case CompanionSocialAction::kBlock:
      return u"已屏蔽该角色。";
    case CompanionSocialAction::kUnblock:
      return u"已解除屏蔽。";
    default:
      return {};
  }
}
}  // namespace
SocialLease::SocialLease(SocialLease&& other) noexcept
    : registry_(std::move(other.registry_)),
      id_(std::exchange(other.id_, 0)),
      token_(std::exchange(other.token_, 0)) {}
SocialLease& SocialLease::operator=(SocialLease&& other) noexcept {
  if (this != &other) {
    Release();
    registry_ = std::move(other.registry_);
    id_ = std::exchange(other.id_, 0);
    token_ = std::exchange(other.token_, 0);
  }
  return *this;
}
SocialLease::~SocialLease() { Release(); }
void SocialLease::Release() noexcept {
  if (registry_) {
    auto r = std::move(registry_);
    r->Leave(id_, token_);
  }
}
void SocialLease::Process(const CompanionSocialRequest& request) {
  if (!registry_) {
    throw std::logic_error("No social membership.");
  }
  registry_->Process(id_, token_, request);
}
void SocialLease::ProcessGroups(const CompanionFriendGroupRequest& request) {
  if (!registry_) {
    throw std::logic_error("No social membership.");
  }
  registry_->ProcessGroups(id_, token_, request);
}
void SocialLease::Block(const ChatBlockRequest& request) {
  if (!registry_) {
    throw std::logic_error("No social membership.");
  }
  registry_->Block(id_, token_, request);
}
void SocialLease::RefreshName() {
  if (registry_) {
    registry_->RefreshName(id_, token_);
  }
}
SocialRegistry::SocialRegistry(StoreFactory factory, Clock clock)
    : factory_(std::move(factory)), clock_(std::move(clock)) {
  if (!factory_) {
    throw std::invalid_argument("Missing social store factory.");
  }
  if (!clock_) {
    clock_ = [] {
      const auto seconds =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      return seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0;
    };
  }
}
void SocialRegistry::Deliver(Member& m,
                             std::vector<TcpPacket> packets) noexcept {
  try {
    m.sink(std::move(packets));
  } catch (...) {
  }
}
SocialRegistry::Member& SocialRegistry::Require(std::uint64_t id,
                                                std::uint64_t token) {
  auto i = members_.find(id);
  if (i == members_.end() || i->second.token != token) {
    throw std::logic_error("Stale social session.");
  }
  return i->second;
}
void SocialRegistry::Refresh(Member& m, SocialStore& store) {
  UseSnapshot(m, store.Load(m.character));
}
void SocialRegistry::UseSnapshot(Member& m, SocialSnapshot snapshot) {
  if (snapshot.owner.character_id != m.character.character_id ||
      snapshot.owner.account_id != m.character.account_id) {
    throw std::runtime_error("Social snapshot owner mismatch.");
  }
  (void)CompanionFriendGroupsNotification{CompanionFriendGroupStatus::kOk,
                                          CompanionFriendGroupAction::kList,
                                          snapshot.groups, snapshot.assignments}
      .Encode();
  for (const auto& assignment : snapshot.assignments) {
    if (std::none_of(snapshot.rows.begin(), snapshot.rows.end(),
                     [&](const auto& row) {
                       return row.character_id == assignment.friend_id &&
                              (row.relationship == 1 || row.relationship == 4);
                     })) {
      throw std::runtime_error("Friend group snapshot lacks its relationship.");
    }
  }
  std::set<std::uint64_t> blocked;
  for (const auto& entry : snapshot.blocks) {
    blocked.insert(entry.character_id);
  }
  {
    std::lock_guard lock(block_mutex_);
    blocks_[m.character.character_id] = std::move(blocked);
  }
  m.character.character_name = snapshot.owner.character_name;
  m.snapshot = std::move(snapshot);
}
void SocialRegistry::Publish(Member& m, CompanionSocialStatus status,
                             CompanionSocialAction action) {
  std::vector<TcpPacket> packets;
  if (m.companion) {
    packets.push_back(Roster(m, status, action).Encode());
  }
  if (m.group_companion) {
    packets.push_back(CompanionFriendGroupsNotification{
        CompanionFriendGroupStatus::kOk, CompanionFriendGroupAction::kList,
        m.snapshot.groups, m.snapshot.assignments}
                          .Encode());
  }
  if (!packets.empty()) {
    Deliver(m, std::move(packets));
  }
}
CompanionSocialRosterNotification SocialRegistry::Roster(
    const Member& m, CompanionSocialStatus status,
    CompanionSocialAction action) {
  CompanionSocialRosterNotification packet{
      status, action, Notice(status, action), m.snapshot.rows};
  for (auto& row : packet.rows) {
    auto online = members_.find(row.character_id);
    // Blocked relationships must not disclose presence.
    row.online = row.relationship != 4 && online != members_.end() &&
                 AllowsPublicChat(row.character_id, m.character.character_id) &&
                 AllowsPublicChat(m.character.character_id, row.character_id);
    if (online != members_.end()) {
      row.name = online->second.character.character_name;
    }
  }
  return packet;
}
void SocialRegistry::RefreshPeers(std::uint64_t id, SocialStore& store) {
  for (auto& [peer_id, peer] : members_) {
    if (peer_id == id) {
      continue;
    }
    if (std::any_of(peer.snapshot.rows.begin(), peer.snapshot.rows.end(),
                    [id](const auto& row) { return row.character_id == id; })) {
      Refresh(peer, store);
      Publish(peer);
    }
  }
}
SocialLease SocialRegistry::Enter(const characters::CharacterRecord& owner,
                                  rooms::RoomPacketSink sink) {
  if (!owner.character_id || owner.character_id > UINT32_MAX ||
      !owner.account_id || !sink) {
    throw std::invalid_argument("Invalid social owner.");
  }
  auto self = shared_from_this();
  std::lock_guard lock(mutex_);
  if (members_.contains(owner.character_id)) {
    throw std::runtime_error("Character is already online.");
  }
  auto store = factory_();
  Member m;
  m.character = owner;
  m.sink = std::move(sink);
  m.token = next_token_++;
  Refresh(m, *store);
  auto [at, inserted] = members_.emplace(owner.character_id, std::move(m));
  (void)inserted;
  try {
    Deliver(at->second,
            {ChatBlockListNotification{at->second.snapshot.blocks}.Encode()});
    for (auto& [id, peer] : members_) {
      if (id != owner.character_id) {
        Publish(peer);
      }
    }
  } catch (...) {
    members_.erase(at);
    std::lock_guard cache(block_mutex_);
    blocks_.erase(owner.character_id);
    throw;
  }
  SocialLease lease;
  lease.registry_ = std::move(self);
  lease.id_ = owner.character_id;
  lease.token_ = at->second.token;
  return lease;
}
void SocialRegistry::Leave(std::uint64_t id, std::uint64_t token) noexcept {
  try {
    std::lock_guard lock(mutex_);
    auto i = members_.find(id);
    if (i == members_.end() || i->second.token != token) {
      return;
    }
    members_.erase(i);
    {
      std::lock_guard cache(block_mutex_);
      blocks_.erase(id);
    }
    for (auto& [peer_id, peer] : members_) {
      (void)peer_id;
      Publish(peer);
    }
  } catch (...) {
  }  // The disconnected identity has already been removed.
}
bool SocialRegistry::AllowsPublicChat(std::uint64_t sender,
                                      std::uint64_t recipient) const {
  if (sender == recipient) {
    return true;
  }
  std::lock_guard lock(block_mutex_);
  const auto i = blocks_.find(recipient);
  return i == blocks_.end() || !i->second.contains(sender);
}
void SocialRegistry::Process(std::uint64_t id, std::uint64_t token,
                             const CompanionSocialRequest& request) {
  std::lock_guard lock(mutex_);
  auto& m = Require(id, token);
  m.companion = true;
  if ((!request.text.empty() && !chat::SceneChat::ValidText(request.text)) ||
      (request.action != CompanionSocialAction::kMessage &&
       request.action != CompanionSocialAction::kAdd &&
       !request.text.empty())) {
    Publish(m, CompanionSocialStatus::kInvalid, request.action);
    return;
  }
  if (request.action == CompanionSocialAction::kMessage &&
      m.mailbox_companion) {
    SendQueued(m, request);
    return;
  }
  auto store = factory_();
  Refresh(m, *store);
  if (request.action == CompanionSocialAction::kList) {
    Publish(m);
    return;
  }
  if (request.action == CompanionSocialAction::kSearch) {
    auto peer = store->Find(request.target_id, request.target_name);
    CompanionSocialRosterNotification result;
    result.action = request.action;
    if (peer && peer->character_id != id) {
      result.rows.push_back({peer->character_id, peer->name, 0, false});
    } else {
      result.status = peer ? CompanionSocialStatus::kSelf
                           : CompanionSocialStatus::kNotFound;
      result.notice = Notice(result.status, request.action);
    }
    Deliver(m, {result.Encode()});
    return;
  }
  if (request.action == CompanionSocialAction::kMessage) {
    if (!chat::SceneChat::ValidText(request.text)) {
      Publish(m, CompanionSocialStatus::kInvalid, request.action);
      return;
    }
    const auto peer = store->Find(request.target_id, request.target_name);
    if (!peer) {
      Publish(m, CompanionSocialStatus::kNotFound, request.action);
      return;
    }
    const auto status = store->CanMessage(m.character, peer->character_id);
    if (status != CompanionSocialStatus::kOk) {
      Publish(m, status, request.action);
      return;
    }
    auto online = members_.find(peer->character_id);
    if (online == members_.end() || !online->second.companion) {
      Publish(m, CompanionSocialStatus::kOffline, request.action);
      return;
    }
    const auto recipient_packet =
        CompanionPrivateMessageNotification{
            static_cast<std::uint32_t>(id), m.character.character_name,
            m.character.character_name, request.text}
            .Encode();
    const auto own_packet =
        CompanionPrivateMessageNotification{peer->character_id, peer->name,
                                            m.character.character_name,
                                            request.text}
            .Encode();
    Deliver(online->second, {recipient_packet});
    Deliver(m, {own_packet});
    return;
  }
  const auto outcome = store->Change(m.character, request);
  Refresh(m, *store);
  if (outcome.status == CompanionSocialStatus::kOk && outcome.target) {
    if (auto peer = members_.find(outcome.target->character_id);
        peer != members_.end()) {
      Refresh(peer->second, *store);
      Publish(peer->second);
    }
    if (request.action == CompanionSocialAction::kBlock ||
        request.action == CompanionSocialAction::kUnblock) {
      Deliver(m, {ChatBlockListNotification{m.snapshot.blocks}.Encode()});
    }
  }
  Publish(m, outcome.status, request.action);
}
void SocialRegistry::ProcessGroups(std::uint64_t id, std::uint64_t token,
                                   const CompanionFriendGroupRequest& request) {
  (void)request.Encode();
  std::lock_guard lock(mutex_);
  auto& member = Require(id, token);
  member.group_companion = true;
  try {
    auto store = factory_();
    auto outcome = store->ChangeGroup(member.character, request);
    UseSnapshot(member, std::move(outcome.snapshot));
    std::vector<TcpPacket> packets;
    if (member.companion) {
      packets.push_back(Roster(member, CompanionSocialStatus::kOk,
                               CompanionSocialAction::kList)
                            .Encode());
    }
    packets.push_back(CompanionFriendGroupsNotification{
        outcome.status, request.action, member.snapshot.groups,
        member.snapshot.assignments}
                          .Encode());
    Deliver(member, std::move(packets));
  } catch (const std::exception&) {
    Deliver(
        member,
        {CompanionFriendGroupsNotification{
            CompanionFriendGroupStatus::kUnavailable, request.action, {}, {}}
             .Encode()});
  }
}
void SocialRegistry::Block(std::uint64_t id, std::uint64_t token,
                           const ChatBlockRequest& request) {
  std::lock_guard lock(mutex_);
  auto& m = Require(id, token);
  auto store = factory_();
  CompanionSocialRequest operation{request.remove
                                       ? CompanionSocialAction::kUnblock
                                       : CompanionSocialAction::kBlock,
                                   request.character_id.value_or(0),
                                   request.name,
                                   {}};
  const auto outcome = store->Change(m.character, operation);
  Refresh(m, *store);
  if (outcome.status == CompanionSocialStatus::kOk && outcome.target) {
    if (request.remove) {
      Deliver(m, {ChatBlockRemovedNotification{*outcome.target}.Encode()});
    } else {
      Deliver(m, {ChatBlockAddedNotification{outcome.target->character_id,
                                             outcome.target->name}
                      .Encode()});
    }
    if (auto peer = members_.find(outcome.target->character_id);
        peer != members_.end()) {
      Refresh(peer->second, *store);
      Publish(peer->second);
    }
  } else {
    if (!request.remove) {
      Deliver(m, {ChatBlockAddedNotification{{}, request.name}.Encode()});
    }
    Deliver(
        m, {chat::SceneChat::Notice(Notice(outcome.status, operation.action))});
  }
  Publish(m);
}
void SocialRegistry::RefreshName(std::uint64_t id, std::uint64_t token) {
  std::lock_guard lock(mutex_);
  auto& m = Require(id, token);
  auto store = factory_();
  Refresh(m, *store);
  RefreshPeers(id, *store);
  Publish(m);
}
}  // namespace bubble_fighter::server::features::social
