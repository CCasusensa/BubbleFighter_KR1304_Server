#include "features/social/room_invitations.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bubble_fighter::server::features::social {
InvitationLease::InvitationLease(std::shared_ptr<RoomInvitations> registry,
                                 std::uint64_t token)
    : registry_(std::move(registry)), token_(token) {}
InvitationLease::InvitationLease(InvitationLease&& other) noexcept
    : registry_(std::move(other.registry_)),
      token_(std::exchange(other.token_, 0)) {}
InvitationLease& InvitationLease::operator=(InvitationLease&& other) noexcept {
  if (this != &other) {
    Release();
    registry_ = std::move(other.registry_);
    token_ = std::exchange(other.token_, 0);
  }
  return *this;
}
InvitationLease::~InvitationLease() { Release(); }
InvitationLease::operator bool() const noexcept { return registry_ != nullptr; }
void InvitationLease::Release() noexcept {
  if (registry_) registry_->Release(token_);
  registry_.reset();
  token_ = 0;
}
void InvitationLease::Update(const characters::CharacterRecord& character,
                             bool available) {
  if (registry_) registry_->Update(token_, character, available);
}
void InvitationLease::Invalidate() {
  if (registry_) registry_->Invalidate(token_);
}
std::vector<protocol::TcpPacket> InvitationLease::Invite(
    const protocol::InvitationRequest& request, Time now) {
  if (!registry_) throw std::logic_error("Missing invitation session.");
  return registry_->Invite(token_, request, now);
}
void InvitationLease::Decline(std::uint32_t key, Time now) {
  if (registry_) registry_->Decline(token_, key, now);
}
std::vector<protocol::TcpPacket> InvitationLease::RequestVisit(
    const protocol::VisitationRequest& request, Time now) {
  if (!registry_) throw std::logic_error("Missing invitation session.");
  return registry_->RequestVisit(token_, request, now);
}
void InvitationLease::DeclineVisit(
    const protocol::DeclineVisitationRequest& request, Time now) {
  // Preserve unknown values in the codec; only observed caller values change
  // state.
  if (registry_ && request.value_at4 <= 1) {
    registry_->DecideVisit(token_, request.visitation_key, false, now);
  }
}
void InvitationLease::ApproveVisit(std::uint32_t key, Time now) {
  if (registry_) registry_->DecideVisit(token_, key, true, now);
}
InvitationAdmission InvitationLease::Accept(
    const protocol::MoveToInviterRequest& request, rooms::RoomOccupant joiner,
    rooms::RoomPacketSink sink, Time now,
    const PartyAdmissionInput* party_input) {
  if (!registry_) throw std::logic_error("Missing invitation session.");
  return registry_->Accept(token_, request, std::move(joiner), std::move(sink),
                           now, party_input);
}

RoomInvitations::RoomInvitations(
    std::shared_ptr<rooms::RoomRegistry> rooms, AllowsPair allows_pair,
    std::shared_ptr<parties::PartyRegistry> parties,
    std::shared_ptr<lobby::PlazaRegistry> plazas,
    clans::ClanPlazaAuthority clan_plaza_authority)
    : rooms_(std::move(rooms)),
      parties_(std::move(parties)),
      plazas_(std::move(plazas)),
      allows_pair_(std::move(allows_pair)),
      clan_plaza_authority_(std::move(clan_plaza_authority)) {
  if (!rooms_) throw std::invalid_argument("Missing invitation room registry.");
}
InvitationLease RoomInvitations::Enter(
    const characters::CharacterRecord& character, rooms::RoomPacketSink sink,
    bool available) {
  if (!character.account_id || !character.character_id || !sink ||
      character.character_name.empty() ||
      character.character_name.size() > 32) {
    throw std::invalid_argument("Invalid invitation identity.");
  }
  std::lock_guard lock(mutex_);
  for (const auto& [token, member] : members_) {
    if (member.character.account_id == character.account_id ||
        member.character.character_id == character.character_id ||
        member.character.character_name == character.character_name) {
      throw std::logic_error("Duplicate invitation session.");
    }
  }
  auto self = shared_from_this();
  const auto token = next_token_++;
  members_.emplace(token, Member{character, std::move(sink), available});
  return InvitationLease(std::move(self), token);
}
void RoomInvitations::Release(std::uint64_t token) noexcept {
  std::lock_guard lock(mutex_);
  std::erase_if(pending_, [token](const auto& item) {
    return item.second.sender == token || item.second.recipient == token;
  });
  members_.erase(token);
}
void RoomInvitations::Update(std::uint64_t token,
                             const characters::CharacterRecord& character,
                             bool available) {
  std::lock_guard lock(mutex_);
  auto& member = members_.at(token);
  if (member.character.account_id != character.account_id ||
      member.character.character_id != character.character_id) {
    throw std::logic_error("Invitation session identity changed.");
  }
  const bool renamed =
      member.character.character_name != character.character_name;
  std::erase_if(pending_, [&](const auto& item) {
    return (renamed && item.second.sender == token) ||
           ((renamed || !available) && item.second.recipient == token);
  });
  member.character = character;
  member.available = available;
  member.transitioning = false;
}
void RoomInvitations::Invalidate(std::uint64_t token) {
  std::lock_guard lock(mutex_);
  auto& member = members_.at(token);
  member.available = false;
  member.transitioning = true;
  std::erase_if(pending_, [token](const auto& item) {
    return item.second.recipient == token ||
           (item.second.sender == token && item.second.target.party);
  });
}
void RoomInvitations::Expire(Time now) {
  std::erase_if(pending_,
                [now](const auto& item) { return now >= item.second.expires; });
}
bool RoomInvitations::Allows(const Member& sender,
                             const Member& recipient) const {
  return !allows_pair_ || allows_pair_(sender.character.character_id,
                                       recipient.character.character_id);
}
void RoomInvitations::Deliver(const Member& member,
                              protocol::TcpPacket packet) noexcept {
  try {
    member.sink({std::move(packet)});
  } catch (...) {
  }
}
std::vector<protocol::TcpPacket> RoomInvitations::Invite(
    std::uint64_t token, const protocol::InvitationRequest& request, Time now) {
  const auto fail = [&](std::uint32_t reason) {
    return std::vector{
        protocol::InvitationFailedNotification{request.target_name, reason}
            .Encode()};
  };
  std::lock_guard lock(mutex_);
  Expire(now);
  const auto& sender = members_.at(token);
  // Bounds/TTL/quotas are server policy. Native input is at most 12 code units.
  if (request.target_name.empty() || request.target_name.size() > 12 ||
      request.value_at24 > 1)
    return fail(20);
  if (sender.character.character_name == request.target_name) return fail(1);
  const auto target = CaptureTarget(sender);
  if (!target) return fail(20);
  const auto target_error = CheckTarget(*target, sender);
  if (target_error) return fail(target_error == 21 ? 20 : target_error);
  const auto found =
      std::find_if(members_.begin(), members_.end(), [&](const auto& item) {
        return item.second.character.character_name == request.target_name;
      });
  if (found == members_.end()) return fail(24);
  const auto& recipient = found->second;
  if (!Allows(sender, recipient)) return fail(26);
  if (rooms_->FindByCharacter(recipient.character.character_id)) return fail(2);
  if (!recipient.available) return fail(3);
  std::size_t count = 0;
  for (const auto& [key, pending] : pending_) {
    if (pending.recipient == found->first) {
      ++count;
      if (pending.sender == token) return fail(20);
    }
  }
  if (count >= 16 || pending_.size() >= 4096 ||
      next_key_ > std::numeric_limits<std::uint32_t>::max())
    return fail(20);
  const auto key = static_cast<std::uint32_t>(next_key_++);
  auto notification =
      protocol::AskedInvitationNotification{
          sender.character.character_name, key, 0, 0, 0, 0}
          .Encode();
  auto delivered = std::vector{
      protocol::InvitationDeliveredNotification{request.target_name}.Encode()};
  pending_.emplace(key, Pending{token, found->first, *target,
                                now + std::chrono::seconds(30)});
  Deliver(recipient, std::move(notification));
  return delivered;
}
void RoomInvitations::Decline(std::uint64_t token, std::uint32_t key,
                              Time now) {
  std::lock_guard lock(mutex_);
  Expire(now);
  const auto pending = pending_.find(key);
  if (pending == pending_.end() || pending->second.recipient != token ||
      pending->second.kind != Pending::Kind::kInvitation)
    return;
  const auto sender = pending->second.sender;
  pending_.erase(pending);
  Deliver(members_.at(sender),
          protocol::InvitationFailedNotification{
              members_.at(token).character.character_name, 26}
              .Encode());
}
InvitationAdmission RoomInvitations::Accept(
    std::uint64_t token, const protocol::MoveToInviterRequest& request,
    rooms::RoomOccupant joiner, rooms::RoomPacketSink sink, Time now,
    const PartyAdmissionInput* party_input) {
  const auto fail = [&](std::uint32_t reason) {
    return InvitationAdmission{
        {},
        {protocol::MoveToInviterFailedNotification{request.inviter_name, reason}
             .Encode()}};
  };
  std::lock_guard lock(mutex_);
  Expire(now);
  const auto found =
      std::find_if(pending_.begin(), pending_.end(), [&](const auto& item) {
        return item.second.recipient == token &&
               members_.at(item.second.sender).character.character_name ==
                   request.inviter_name;
      });
  if (found == pending_.end()) return fail(21);
  // An application to visit is not admission authority until its recipient
  // confirms C114. C115 alone cannot grant that consent.
  if (found->second.kind == Pending::Kind::kVisitRequest) return fail(21);
  const auto pending = found->second;
  pending_.erase(
      found);  // One attempt consumes authority, including invalid echoes.
  auto& recipient = members_.at(token);
  const auto& sender = members_.at(pending.sender);
  if (!recipient.available || request.address != 0 || request.port != 0 ||
      request.byte_at32 !=
          (pending.kind == Pending::Kind::kInvitation ? 1 : 0) ||
      !Allows(sender, recipient) ||
      joiner.account_id != recipient.character.account_id ||
      joiner.character != recipient.character ||
      rooms_->FindByCharacter(recipient.character.character_id))
    return fail(21);
  joiner.member.booleans_at96 = {};
  if (pending.target.party) {
    if (!party_input) return fail(21);
    const auto reason = CheckTarget(pending.target, sender);
    if (reason) return fail(reason);
    auto admitted =
        AcceptParty(pending.target, std::move(joiner), *party_input);
    if (admitted.plaza || admitted.moved_to_menu) {
      // The owning connection still has to adopt the replacement lease/queue
      // (or release its old plaza). Do not expose that short interval as a
      // new invitation destination. SyncInvitations reopens it afterward.
      recipient.available = false;
      recipient.transitioning = true;
      std::erase_if(pending_, [token](const auto& item) {
        return item.second.recipient == token || item.second.sender == token;
      });
    }
    return admitted;
  }
  auto result = rooms_->JoinInvited(pending.target.room, std::move(joiner),
                                    std::move(sink));
  if (result.status == rooms::RoomJoinStatus::kJoined) {
    recipient.available = false;
    std::erase_if(pending_, [token](const auto& item) {
      return item.second.recipient == token;
    });
    return {std::move(result.room), {}};
  }
  return fail(result.status == rooms::RoomJoinStatus::kPlaying ? 8U
              : result.status == rooms::RoomJoinStatus::kFull  ? 15U
                                                               : 21U);
}

std::vector<protocol::TcpPacket> RoomInvitations::RequestVisit(
    std::uint64_t token, const protocol::VisitationRequest& request, Time now) {
  const auto fail = [&](std::uint32_t reason) {
    return std::vector{
        protocol::VisitationFailedNotification{request.target_name, reason}
            .Encode()};
  };
  std::lock_guard lock(mutex_);
  Expire(now);
  const auto& visitor = members_.at(token);
  if (request.target_name.empty() || request.target_name.size() > 12)
    return fail(20);
  if (request.target_name == visitor.character.character_name) return fail(1);
  const auto found =
      std::find_if(members_.begin(), members_.end(), [&](const auto& item) {
        return item.second.character.character_name == request.target_name;
      });
  if (found == members_.end()) return fail(21);
  const auto& owner = found->second;
  if (!Allows(owner, visitor)) return fail(28);
  const auto target = CaptureTarget(owner);
  if (!target) return fail(20);
  const auto visitor_room =
      rooms_->FindByCharacter(visitor.character.character_id);
  if (!target->party && visitor_room == target->room.room_id) return fail(2);
  if (visitor_room || !visitor.available) return fail(3);
  const auto target_error = CheckTarget(*target, owner);
  if (target_error) return fail(target_error == 21 ? 20 : target_error);
  std::size_t visitor_count = 0, owner_count = 0;
  for (const auto& [key, pending] : pending_) {
    if (pending.recipient == token) {
      ++visitor_count;
      if (pending.sender == found->first) return fail(20);
    }
    if (pending.sender == found->first &&
        pending.kind == Pending::Kind::kVisitRequest)
      ++owner_count;
  }
  if (visitor_count >= 16 || owner_count >= 16 || pending_.size() >= 4096 ||
      next_key_ > std::numeric_limits<std::uint32_t>::max())
    return fail(20);
  const auto key = static_cast<std::uint32_t>(next_key_++);
  auto asked =
      protocol::AskedVisitationNotification{key,
                                            visitor.character.character_name, 0}
          .Encode();
  pending_.emplace(
      key, Pending{found->first, token, *target, now + std::chrono::seconds(30),
                   Pending::Kind::kVisitRequest});
  Deliver(owner, std::move(asked));
  return {};  // No native visitation-delivered notification is established.
}

void RoomInvitations::DecideVisit(std::uint64_t token, std::uint32_t key,
                                  bool approved, Time now) {
  std::lock_guard lock(mutex_);
  Expire(now);
  const auto found = pending_.find(key);
  if (found == pending_.end() || found->second.sender != token ||
      found->second.kind != Pending::Kind::kVisitRequest)
    return;
  const auto& owner = members_.at(token);
  const auto& visitor = members_.at(found->second.recipient);
  const auto fail = [&](std::uint32_t reason) {
    auto packet =
        protocol::VisitationFailedNotification{owner.character.character_name,
                                               reason}
            .Encode();
    pending_.erase(found);
    Deliver(visitor, std::move(packet));
  };
  if (!approved || !Allows(owner, visitor)) {
    fail(28);
    return;
  }
  if (!visitor.available ||
      rooms_->FindByCharacter(visitor.character.character_id)) {
    fail(3);
    return;
  }
  const auto target_error = CheckTarget(found->second.target, owner);
  if (target_error) {
    fail(target_error == 21 ? 20 : target_error);
    return;
  }
  auto packet =
      protocol::VisitationAcceptedNotification{owner.character.character_name,
                                               0, 0}
          .Encode();
  found->second.kind = Pending::Kind::kApprovedVisit;
  // Keep the original expiry; approval does not extend a stale application.
  Deliver(visitor, std::move(packet));
}
}  // namespace bubble_fighter::server::features::social
