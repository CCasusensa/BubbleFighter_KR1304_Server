#include <stdexcept>
#include <utility>

#include "features/scenes/scene_roster.h"
#include "protocol/scenes/channel_transfer.h"
#include "sessions/player_session.h"

namespace bubble_fighter::server::sessions {
struct PlayerSession::ChannelTransfer {
  std::shared_ptr<const features::channels::ChannelWorld> destination;
  std::optional<features::parties::PartySceneReservation> party;
  MenuInitializationData data;
  std::shared_ptr<features::rooms::RoomPacketQueue> packets =
      std::make_shared<features::rooms::RoomPacketQueue>();
  features::parties::PartySceneConnection scene;
  features::social::InvitationLease invitation;
  std::shared_ptr<features::channels::ChannelMovePrepared> collective;
};
std::shared_ptr<PlayerSession::ChannelTransfer>
PlayerSession::PrepareChannelDestination(
    std::shared_ptr<const features::channels::ChannelWorld> destination) {
  auto prepared = std::make_shared<ChannelTransfer>();
  prepared->destination = std::move(destination);
  prepared->data = LoadMenuData(InventoryObservationTime());
  auto member = features::scenes::SceneSlot{prepared->data.member, *character_,
                                            prepared->data.equipment};
  features::scenes::SceneRoster::Compose(prepared->data.scene, {&member, 1}, 0,
                                         *account_id_)
      .Encode();
  prepared->scene =
      prepared->destination->transfers->Register(party_membership_);
  prepared->invitation = prepared->destination->invitations->Enter(
      *character_, social_packets_->Sink(), false);
  prepared->invitation.Invalidate();
  return prepared;
}
void PlayerSession::EnableChannelHandoff() { channel_connection_.Enable(); }
bool PlayerSession::PartyChannelIrreversible() const {
  return channel_connection_.Irreversible();
}
void PlayerSession::CancelPartyChannelHandoff() noexcept {
  channel_connection_.Cancel();
}
bool PlayerSession::SignalPartyChannelHandoff(
    features::channels::ChannelMoveSignal signal) {
  return channel_connection_.Signal(signal, practice_clock_());
}
std::optional<bool> PlayerSession::BeginPartyChannelHandoff(
    std::uint32_t category) {
  channel_refusal_ = 2;
  CheckClanChannel();
  if (category == channel_category_ || !party_membership_ ||
      party_membership_.Snapshot().members.size() < 2)
    return std::nullopt;
  if (!channels_ || !account_id_ || !character_ || !CommerceInitialized() ||
      practice_ || scene_connection_.Pending() || connection_handoff_)
    return false;
  SyncParty();
  const auto began = channel_connection_.Begin(
      party_membership_, channels_->Find(channel_category_),
      channels_->Find(category), practice_clock_());
  if (!began) channel_refusal_ = channel_connection_.Refusal();
  return began;
}
features::channels::ChannelMoveStatus PlayerSession::PollPartyChannelHandoff() {
  using namespace features::channels;
  auto status = channel_connection_.Status(practice_clock_());
  if (status.phase != ChannelMovePhase::kPreparing) return status;
  InvalidateClanScene();
  try {
    if (!account_id_ || !character_ || !CommerceInitialized() || practice_ ||
        connection_handoff_ || scene_connection_.Pending() || !characters_ ||
        !menu_data_)
      throw std::runtime_error("Party channel source is unavailable.");
    if (room_) {
      const auto room = rooms_->Find(room_.RoomId());
      if (!room || room->state != features::rooms::RoomState::kWaiting)
        throw std::runtime_error("Party channel source is unavailable.");
    }
    const auto current = characters_->FindByAccount(*account_id_);
    if (!current || current->account_id != *account_id_ ||
        current->character_id != character_->character_id ||
        !status.destination->Allows(*current))
      throw std::runtime_error("Party channel member is ineligible.");
    character_ = *current;
    auto prepared = PrepareChannelDestination(status.destination);
    SyncParty();
    auto collective = std::make_shared<ChannelMovePrepared>();
    collective->character = *character_;
    collective->envelope = prepared->data.scene;
    collective->member = prepared->data.member;
    collective->equipment = prepared->data.equipment;
    collective->packets = prepared->packets;
    collective->source_room_id = room_.RoomId();
    collective->source = std::move(plaza_);
    collective->source_room = std::move(room_);
    prepared->collective = collective;
    channel_transfer_ = prepared;
    connection_handoff_ = true;
    if (invitation_lease_) invitation_lease_.Invalidate();
    if (!channel_connection_.Prepare(status.id, collective,
                                     practice_clock_())) {
      plaza_ = std::move(collective->source);
      room_ = std::move(collective->source_room);
      channel_transfer_.reset();
      connection_handoff_ = false;
      channel_connection_.Cancel();
    }
  } catch (...) {
    channel_connection_.Cancel();
  }
  return channel_connection_.Status(practice_clock_());
}
std::uint32_t PlayerSession::ChannelCategory() const noexcept {
  return channel_category_;
}
std::uint32_t PlayerSession::ChannelRefusal() const noexcept {
  return channel_refusal_;
}
bool PlayerSession::BeginConnectionHandoff(std::uint32_t category) {
  channel_refusal_ = 2;
  CheckClanChannel();
  if (channel_connection_.Pending()) {
    const auto status = channel_connection_.Status(practice_clock_());
    if (status.phase != features::channels::ChannelMovePhase::kOffer ||
        status.category != category || !channel_transfer_ ||
        !channel_transfer_->collective)
      return false;
    return channel_connection_.Signal(
        features::channels::ChannelMoveSignal::kOffered, practice_clock_());
  }
  if (!account_id_ || !character_ || !CommerceInitialized() || practice_ ||
      scene_connection_.Pending() || connection_handoff_)
    return false;
  if (category != channel_category_) {
    if (!channels_ || !parties_ || !party_membership_ || !characters_)
      return false;
    const auto destination = channels_->Find(category);
    if (!destination) return false;
    if (room_) {
      const auto current = rooms_->Find(room_.RoomId());
      if (!current || current->state != features::rooms::RoomState::kWaiting)
        return false;
    }
    // Read trusted current progression, never use C54's UI value as
    // eligibility.
    const auto current = characters_->FindByAccount(*account_id_);
    if (!current || current->account_id != *account_id_ ||
        current->character_id != character_->character_id ||
        !destination->Allows(*current))
      return false;
    character_ = *current;
    SyncParty();
    auto reservation = parties_->ReserveScene(party_membership_, true);
    // Multi-member movement must use the collective coordinator.
    if (!reservation || reservation->Members().size() != 1) return false;
    const auto decision = destination->Authorize({&*character_, 1});
    if (decision.result != 0) {
      channel_refusal_ = decision.result;
      return false;
    }
    auto prepared = PrepareChannelDestination(destination);
    prepared->party = std::move(reservation);
    channel_transfer_ = std::move(prepared);
  }
  connection_handoff_ = true;
  SyncParty();
  InvalidateClanScene();
  if (invitation_lease_) invitation_lease_.Invalidate();
  return true;
}
void PlayerSession::EndConnectionHandoff() {
  if (channel_connection_.Pending()) {
    channel_connection_.Cancel();
    auto outcome = channel_connection_.Take(practice_clock_());
    if (!outcome || outcome->committed || outcome->closed)
      throw std::logic_error("Cannot restore a switched party channel.");
    auto prepared = outcome->prepared   ? outcome->prepared
                    : channel_transfer_ ? channel_transfer_->collective
                                        : nullptr;
    if (prepared) {
      plaza_ = std::move(prepared->source);
      room_ = std::move(prepared->source_room);
    }
  }
  channel_transfer_.reset();
  connection_handoff_ = false;
  SyncParty();
  SyncInvitations();
}
std::vector<protocol::TcpPacket> PlayerSession::CompleteConnectionHandoff() {
  if (!connection_handoff_)
    throw std::logic_error("Channel commit without preparation.");
  if (!channel_transfer_) {
    EndConnectionHandoff();
    return {};
  }
  auto& prepared = *channel_transfer_;
  features::lobby::PlazaLease admitted;
  if (prepared.collective) {
    auto outcome = channel_connection_.Take(practice_clock_());
    if (!outcome || !outcome->committed)
      throw std::logic_error("Party channel commit is incomplete.");
    admitted = std::move(outcome->plaza);
  } else {
    auto admit = [&] {
      features::scenes::SceneSlot member{prepared.data.member, *character_,
                                         prepared.data.equipment};
      admitted = prepared.destination->plazas->Enter(
          prepared.data.scene, std::move(member), *account_id_,
          prepared.packets->Sink());
      return static_cast<bool>(admitted);
    };
    // Party reservation -> source room -> target plaza. No target room lock is
    // needed, so opposite-direction migrations cannot form a room-lock cycle.
    const bool committed = prepared.party->Commit([&] {
      return prepared.destination
                 ->Authorize(
                     {&*character_, 1},
                     [&] {
                       if (room_)
                         return rooms_->DepartGroup(
                             {{&room_,
                               {*account_id_, character_->character_id}}},
                             admit);
                       return admit();
                     },
                     true)
                 .result == 0;
    });
    if (!committed)
      throw std::runtime_error("Channel source changed before commit.");
  }
  plaza_.Release();
  plaza_packets_->Clear();
  room_packets_->Clear();
  invitation_lease_.Release();
  scene_connection_.Release();
  const auto destination = prepared.destination;
  rooms_ = destination->rooms;
  plazas_ = destination->plazas;
  invitations_ = destination->invitations;
  scene_transfers_ = destination->transfers;
  channel_category_ = destination->category;
  channel_poll_at_.reset();
  channel_connection_.SetCategory(channel_category_);
  scene_connection_ = std::move(prepared.scene);
  invitation_lease_ = std::move(prepared.invitation);
  plaza_ = std::move(admitted);
  plaza_packets_ = prepared.packets;
  lobby_.emplace();
  lobby_->SnapshotPublished();
  state_ = PlayerSessionState::kMenuInitializationSent;
  EndConnectionHandoff();
  return plaza_packets_->Drain();
}
std::optional<std::vector<protocol::TcpPacket>> PlayerSession::ProcessChannel(
    const protocol::TcpPacket& packet) {
  using namespace protocol;
  if (packet.message_id == MoveChannelRequest::kMessageId) {
    const auto request = MoveChannelRequest::Decode(packet);
    if (!account_id_ || !character_ || !lobby_ ||
        (!CommerceInitialized() && !practice_)) {
      throw std::runtime_error("Channel selection before initialization.");
    }
    // Unsupported destinations and connections without the companion opt-in
    // reach this fallback. Authorized same-category rebinds are intercepted
    // by ConnectionHandoffs; preserve all source leases here.
    return std::vector<TcpPacket>{MoveChannelResponse{
        MoveChannelResponse::kUnavailable, {}, 0, request.channel_category}
                                      .Encode()};
  }
  if (packet.message_id == MoveIntoRequest::kMessageId) {
    MoveIntoRequest::Decode(packet);
    throw std::runtime_error("Channel entry requires an authorized handoff.");
  }
  if (packet.message_id == ChannelSwitchAcknowledgementRequest::kMessageId) {
    ChannelSwitchAcknowledgementRequest::Decode(packet);
    throw std::runtime_error("Channel acknowledgement without a handoff.");
  }
  return std::nullopt;
}
}  // namespace bubble_fighter::server::sessions
