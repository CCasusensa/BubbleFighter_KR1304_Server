#include <stdexcept>
#include <utility>

#include "features/scenes/scene_roster.h"
#include "sessions/player_session.h"

namespace bubble_fighter::server::sessions {
MenuInitializationData PlayerSession::LoadMenuData(std::uint64_t observed_at) {
  auto data = menu_data_(*character_, observed_at);
  if (party_membership_) {
    if (data.inventory.account_id != *account_id_ ||
        data.inventory.character_id != character_->character_id ||
        data.equipment.key != character_->character_id)
      throw std::invalid_argument(
          "Party equipment source does not match authenticated ownership.");
    // Validate the shared S117/S806 record before caching it. Room inventory
    // callbacks only update this cache; publish after scene operations finish.
    auto view = features::scenes::SceneRoster::CreateInventoryView(
        *account_id_, *character_, data.equipment);
    view.Encode();
    party_equipment_ = std::move(view.view);
  }
  return data;
}
void PlayerSession::BeginSceneChange() {
  InvalidateClanScene();
  // Publish busy before changing the real scene. Public party admission holds
  // the party lock through scene commit, so these operations cannot overtake
  // it.
  if (party_membership_ && character_)
    party_membership_.Update(*character_, true, party_equipment_);
  if (invitation_lease_) invitation_lease_.Invalidate();
}
void PlayerSession::SyncParty() {
  RefreshPartyClan();
  if (!party_membership_ || !character_) return;
  bool busy = !CommerceInitialized() || practice_.has_value();
  if (room_) {
    const auto current = rooms_->Find(room_.RoomId());
    busy = !current || current->state != features::rooms::RoomState::kWaiting;
  } else if (scene_prepared_ && scene_prepared_->source_room_id != 0) {
    // The lease is owned by the coordinator until commit/rollback. Only read
    // its immutable identity; another participant may be committing now.
    const auto current = rooms_->Find(scene_prepared_->source_room_id);
    busy = !current || current->state != features::rooms::RoomState::kWaiting;
  }
  busy = busy || (connection_handoff_ && !channel_transfer_);
  party_membership_.Update(*character_, busy, party_equipment_, party_clan_id_);
}
std::optional<std::vector<protocol::TcpPacket>> PlayerSession::ProcessParty(
    const protocol::TcpPacket& packet) {
  using namespace protocol;
  if (packet.message_id != LeavePartyRequest::kMessageId &&
      packet.message_id != KickPartyRequest::kMessageId &&
      packet.message_id != ChangePartyLeaderRequest::kMessageId)
    return std::nullopt;
  if (!party_membership_ || !account_id_ || !character_ || !menu_data_ ||
      (!CommerceInitialized() && !practice_))
    throw std::runtime_error(
        "Party operation before authenticated initialization.");
  SyncParty();
  if (packet.message_id == LeavePartyRequest::kMessageId) {
    LeavePartyRequest::Decode(packet);
    party_membership_.Leave();
    return std::vector<TcpPacket>{};
  }
  if (packet.message_id == KickPartyRequest::kMessageId) {
    const auto request = KickPartyRequest::Decode(packet);
    if (!chat_.Admit(practice_clock_()))
      return std::vector{
          features::chat::SceneChat::Notice(u"操作过于频繁，请稍后重试。")};
    party_membership_.Kick(request.target_name);
  } else {
    const auto request = ChangePartyLeaderRequest::Decode(packet);
    if (!chat_.Admit(practice_clock_()))
      return std::vector{
          features::chat::SceneChat::Notice(u"操作过于频繁，请稍后重试。")};
    party_membership_.ChangeLeader(request.target_name);
  }
  return std::vector<TcpPacket>{};
}
}  // namespace bubble_fighter::server::sessions
