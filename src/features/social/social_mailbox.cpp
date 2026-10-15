#include <algorithm>
#include <stdexcept>

#include "features/chat/scene_chat.h"
#include "features/social/social_registry.h"

namespace bubble_fighter::server::features::social {
using namespace protocol;
void SocialLease::ProcessMailbox(const CompanionMailboxRequest& request) {
  if (!registry_) {
    throw std::logic_error("No social membership.");
  }
  registry_->ProcessMailbox(id_, token_, request);
}

void SocialRegistry::RememberAcknowledgement(Member& member, std::uint64_t id) {
  member.mailbox_acknowledged.push_back(id);
  if (member.mailbox_acknowledged.size() > kMailboxCapacity) {
    member.mailbox_acknowledged.pop_front();
  }
}

void SocialRegistry::OfferMailbox(Member& member, SocialStore& store) {
  auto mailbox = store.LoadMailbox(member.character, clock_());
  auto notification =
      CompanionMailboxNotification{mailbox.status, mailbox.messages}.Encode();
  if (mailbox.status != CompanionMailboxStatus::kOk) {
    Deliver(member, {std::move(notification)});
    return;
  }
  for (const auto& message : mailbox.messages) {
    if (message.sender_id == member.character.character_id ||
        !chat::SceneChat::ValidText(message.text) ||
        std::none_of(mailbox.snapshot.rows.begin(), mailbox.snapshot.rows.end(),
                     [&](const auto& row) {
                       return row.character_id == message.sender_id &&
                              row.relationship == 1 &&
                              row.name == message.sender_name;
                     })) {
      throw std::runtime_error("Mailbox snapshot lacks its friendship.");
    }
  }
  UseSnapshot(member, std::move(mailbox.snapshot));
  std::vector<TcpPacket> packets;
  if (member.companion) {
    packets.push_back(
        Roster(member, CompanionSocialStatus::kOk, CompanionSocialAction::kList)
            .Encode());
  }
  if (member.group_companion) {
    packets.push_back(CompanionFriendGroupsNotification{
        CompanionFriendGroupStatus::kOk, CompanionFriendGroupAction::kList,
        member.snapshot.groups, member.snapshot.assignments}
                          .Encode());
  }
  packets.push_back(std::move(notification));
  std::set<std::uint64_t> issued;
  for (const auto& message : mailbox.messages)
    issued.insert(message.message_id);
  // Revoked/expired records may already have been handed to the adapter.
  // Permit their late acknowledgements as bounded no-ops; never re-delete them.
  for (auto id : member.mailbox_inflight) {
    if (!issued.contains(id)) RememberAcknowledgement(member, id);
  }
  member.mailbox_inflight = std::move(issued);
  Deliver(member, std::move(packets));
}

void SocialRegistry::ProcessMailbox(std::uint64_t id, std::uint64_t token,
                                    const CompanionMailboxRequest& request) {
  (void)request.Encode();
  std::lock_guard lock(mutex_);
  auto& member = Require(id, token);
  if (request.action == CompanionMailboxAction::kAcknowledge) {
    if (!member.mailbox_companion) {
      throw std::invalid_argument("Mailbox acknowledgement before opt-in.");
    }
    if (!member.mailbox_inflight.contains(request.message_id)) {
      if (std::find(member.mailbox_acknowledged.begin(),
                    member.mailbox_acknowledged.end(),
                    request.message_id) != member.mailbox_acknowledged.end()) {
        return;
      }
      throw std::invalid_argument("Mailbox acknowledgement was not issued.");
    }
  } else {
    member.mailbox_companion = true;
  }
  try {
    auto store = factory_();
    if (request.action == CompanionMailboxAction::kAcknowledge) {
      store->AcknowledgeMessage(member.character, request.message_id);
      member.mailbox_inflight.erase(request.message_id);
      RememberAcknowledgement(member, request.message_id);
      if (!member.mailbox_inflight.empty()) return;
    }
    OfferMailbox(member, *store);
  } catch (const std::exception&) {
    Deliver(member, {CompanionMailboxNotification{
                        CompanionMailboxStatus::kUnavailable, {}}
                         .Encode()});
  }
}

void SocialRegistry::SendQueued(Member& member,
                                const CompanionSocialRequest& request) {
  if (!chat::SceneChat::ValidText(request.text)) {
    Publish(member, CompanionSocialStatus::kInvalid, request.action);
    return;
  }
  try {
    auto store = factory_();
    const auto result =
        store->QueueMessage(member.character, request, clock_());
    if (result.status != CompanionSocialStatus::kOk) {
      Publish(member, result.status, request.action);
      return;
    }
    if (!result.target || !result.message_id || result.sender_name.empty()) {
      throw std::runtime_error("Invalid committed mailbox message.");
    }
    Deliver(member, {CompanionPrivateMessageNotification{
                         result.target->character_id, result.target->name,
                         result.sender_name, request.text}
                         .Encode(),
                     chat::SceneChat::Notice(u"消息已保存，等待对方接收。")});
    auto peer = members_.find(result.target->character_id);
    if (peer != members_.end() && peer->second.mailbox_companion &&
        peer->second.mailbox_inflight.empty()) {
      // The commit succeeded even if the recipient's subsequent read fails.
      // Never report a send failure that might invite duplicate submissions.
      try {
        OfferMailbox(peer->second, *store);
      } catch (const std::exception&) {
        Deliver(peer->second, {CompanionMailboxNotification{
                                  CompanionMailboxStatus::kUnavailable, {}}
                                   .Encode()});
      }
    }
  } catch (const std::exception&) {
    Publish(member, CompanionSocialStatus::kUnavailable, request.action);
  }
}
}  // namespace bubble_fighter::server::features::social
