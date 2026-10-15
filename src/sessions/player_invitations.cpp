#include <stdexcept>
#include <utility>

#include "sessions/player_session.h"

namespace bubble_fighter::server::sessions {
void PlayerSession::SyncInvitations() {
  SyncClanScene();
  if (!invitations_ || !character_) return;
  if (scene_connection_.Pending() || channel_connection_.Pending() ||
      connection_handoff_) {
    // Update clears the transition flag even for unavailable members. Keep
    // committed destinations hidden until this connection adopts its lease.
    if (invitation_lease_) invitation_lease_.Invalidate();
    return;
  }
  const bool available =
      state_ == PlayerSessionState::kMenuInitializationSent && !practice_ &&
      !room_;
  if (!invitation_lease_ && available) {
    invitation_lease_ =
        invitations_->Enter(*character_, social_packets_->Sink(), true);
  } else if (invitation_lease_) {
    invitation_lease_.Update(*character_, available);
  }
}

std::optional<std::vector<protocol::TcpPacket>>
PlayerSession::ProcessInvitation(const protocol::TcpPacket& packet) {
  using namespace protocol;
  if (packet.message_id != InvitationRequest::kMessageId &&
      packet.message_id != DeclineInvitationRequest::kMessageId &&
      packet.message_id != MoveToInviterRequest::kMessageId &&
      packet.message_id != VisitationRequest::kMessageId &&
      packet.message_id != DeclineVisitationRequest::kMessageId &&
      packet.message_id != AcceptVisitationRequest::kMessageId)
    return std::nullopt;
  if (!invitation_lease_ || !account_id_ || !character_ || !menu_data_ ||
      (!CommerceInitialized() && !practice_)) {
    throw std::runtime_error(
        "Invitation request before authenticated initialization.");
  }
  SyncInvitations();
  const auto now = practice_clock_();
  if (packet.message_id == VisitationRequest::kMessageId) {
    const auto request = VisitationRequest::Decode(packet);
    if (!chat_.Admit(now)) {
      return std::vector{
          VisitationFailedNotification{request.target_name, 20}.Encode()};
    }
    return invitation_lease_.RequestVisit(request, now);
  }
  if (packet.message_id == DeclineVisitationRequest::kMessageId) {
    invitation_lease_.DeclineVisit(DeclineVisitationRequest::Decode(packet),
                                   now);
    return std::vector<TcpPacket>{};
  }
  if (packet.message_id == AcceptVisitationRequest::kMessageId) {
    const auto request = AcceptVisitationRequest::Decode(packet);
    if (chat_.Admit(now))
      invitation_lease_.ApproveVisit(request.visitation_key, now);
    return std::vector<TcpPacket>{};
  }
  if (packet.message_id == InvitationRequest::kMessageId) {
    const auto request = InvitationRequest::Decode(packet);
    if (!chat_.Admit(now)) {
      return std::vector{
          InvitationFailedNotification{request.target_name, 20}.Encode()};
    }
    return invitation_lease_.Invite(request, now);
  }
  if (packet.message_id == DeclineInvitationRequest::kMessageId) {
    const auto request = DeclineInvitationRequest::Decode(packet);
    // Declines only remove existing authority and do not spend the send quota.
    invitation_lease_.Decline(request.invitation_key, now);
    return std::vector<TcpPacket>{};
  }
  const auto request = MoveToInviterRequest::Decode(packet);
  if (room_ || practice_ || !chat_.Admit(now)) {
    return std::vector{
        MoveToInviterFailedNotification{request.inviter_name, 21}.Encode()};
  }
  InvalidateClanScene();
  const auto data = LoadMenuData(InventoryObservationTime());
  // JoinApproved encodes the complete destination roster. Refresh this
  // member's cached appearance before it becomes part of that roster.
  SyncParty();
  features::rooms::RoomOccupant joiner;
  joiner.account_id = *account_id_;
  joiner.character = *character_;
  joiner.member = data.member;
  joiner.equipment = data.equipment;
  // A new queue prevents late packets for the previous plaza from following
  // the committed menu/plaza snapshot into its replacement context.
  auto admitted_scene_packets =
      std::make_shared<features::rooms::RoomPacketQueue>();
  features::social::PartyAdmissionInput party_input{
      &party_membership_, data.scene, admitted_scene_packets->Sink(),
      plaza_.Token()};
  if (channel_category_ == 80 && channels_) {
    const auto world = channels_->Find(80);
    party_input.authority = [world](auto members, const auto& commit) {
      return world && world->Authorize(members, commit).result == 0;
    };
  }
  auto admission = invitation_lease_.Accept(
      request, std::move(joiner), room_packets_->Sink(), now, &party_input);
  if (admission.room) {
    plaza_.Release();
    plaza_packets_->Clear();
    room_ = std::move(admission.room);
    state_ = PlayerSessionState::kInRoom;
  } else if (admission.plaza) {
    plaza_ = std::move(admission.plaza);
    plaza_packets_ = std::move(admitted_scene_packets);
    lobby_->SnapshotPublished();
    state_ = PlayerSessionState::kMenuInitializationSent;
  } else if (admission.moved_to_menu) {
    plaza_.Release();
    plaza_packets_ = std::move(admitted_scene_packets);
    lobby_.emplace();
    state_ = PlayerSessionState::kMenuInitializationSent;
  }
  return std::move(admission.responses);
}
}  // namespace bubble_fighter::server::sessions
