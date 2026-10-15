#include <utility>

#include "protocol/clans/clan_plaza_request.h"
#include "protocol/rooms/create_room_request.h"
#include "protocol/rooms/join_room_request.h"
#include "protocol/rooms/leave_room_request.h"
#include "protocol/unresolved/client_to_server_message_0x0069.h"
#include "protocol/unresolved/client_to_server_message_0x006a.h"
#include "sessions/player_session.h"

namespace bubble_fighter::server::sessions {
std::optional<std::vector<protocol::TcpPacket>>
PlayerSession::ProcessPartyScene(const protocol::TcpPacket& packet) {
  using namespace features::parties;
  using namespace protocol;
  const bool clan_plaza = packet.message_id == ClanPlazaRequest::kMessageId;
  ClanPlazaRequest clan_request;
  if (clan_plaza) {
    clan_request = ClanPlazaRequest::Decode(packet);
    if (!account_id_ || !character_ || !clan_game_)
      throw std::runtime_error("Clan plaza requires a trusted game character.");
    const auto source_room =
        room_ ? rooms_->Find(room_.RoomId()) : std::nullopt;
    const bool waiting =
        state_ == PlayerSessionState::kInRoom && source_room &&
        source_room->state == features::rooms::RoomState::kWaiting;
    const bool outside =
        state_ == PlayerSessionState::kMenuInitializationSent && !room_;
    if (!clan_request.reported_clan_id ||
        clan_request.reported_clan_id > 0x7FFFFFFFULL || !scene_connection_ ||
        !party_membership_ || practice_ || !menu_data_ || !lobby_ ||
        (!outside && !waiting))
      return std::vector{features::chat::SceneChat::Notice(
          u"当前无法进入战队广场，请检查队伍及战队资格。")};
  }
  if (!scene_connection_ || !party_membership_ ||
      (!clan_plaza && packet.message_id != 105 && packet.message_id != 106 &&
       packet.message_id != CreateRoomRequest::kMessageId &&
       packet.message_id != JoinRoomRequest::kMessageId &&
       packet.message_id != LeaveRoomRequest::kMessageId) ||
      (!clan_plaza && party_membership_.Snapshot().members.size() < 2))
    return std::nullopt;
  const bool leaving = packet.message_id == LeaveRoomRequest::kMessageId;
  // Preserve existing single-session guards for malformed source scenes.
  if (practice_ || !menu_data_ || !character_ || !account_id_ || !lobby_ ||
      (!clan_plaza &&
       (leaving
            ? state_ != PlayerSessionState::kInRoom || !room_
            : state_ != PlayerSessionState::kMenuInitializationSent || room_)))
    return std::nullopt;
  PartySceneRequest request;
  if (clan_plaza) {
    request.kind = PartySceneKind::kClanPlaza;
    request.clan_id = static_cast<std::uint32_t>(clan_request.reported_clan_id);
  } else if (leaving) {
    LeaveRoomRequest::Decode(packet);
    request.kind = PartySceneKind::kLeaveRoom;
  } else if (packet.message_id == 105) {
    ClientToServerMessage0x0069::Decode(packet);
    request.kind = PartySceneKind::kMenu;
  } else if (packet.message_id == 106) {
    ClientToServerMessage0x006A::Decode(packet);
    request.kind = PartySceneKind::kPlaza;
  } else {
    if (!plaza_) return std::nullopt;
    if (packet.message_id == CreateRoomRequest::kMessageId) {
      request.kind = PartySceneKind::kCreateRoom;
      request.create = CreateRoomRequest::Decode(packet);
    } else {
      request.kind = PartySceneKind::kJoinRoom;
      request.join = JoinRoomRequest::Decode(packet);
    }
  }
  SyncParty();
  if (!scene_connection_.Begin(party_membership_, std::move(request),
                               practice_clock_())) {
    return std::vector{features::chat::SceneChat::Notice(
        u"当前无法整队移动：请由队长发起，并等待所有成员就绪。")};
  }
  return PollPartyScene();
}

std::vector<protocol::TcpPacket> PlayerSession::PollPartyScene() {
  using namespace features::parties;
  if (!scene_connection_) return {};
  if (const auto work = scene_connection_.Work(practice_clock_())) {
    const bool room_entry = work->request.kind == PartySceneKind::kCreateRoom ||
                            work->request.kind == PartySceneKind::kJoinRoom;
    const bool leaving = work->request.kind == PartySceneKind::kLeaveRoom ||
                         work->request.kind == PartySceneKind::kClanPlaza;
    const auto source_room =
        room_ ? rooms_->Find(room_.RoomId()) : std::nullopt;
    const bool waiting =
        state_ == PlayerSessionState::kInRoom && source_room &&
        source_room->state == features::rooms::RoomState::kWaiting;
    const bool outside =
        state_ == PlayerSessionState::kMenuInitializationSent && !room_;
    if ((!outside && !(leaving && waiting)) || practice_ || !account_id_ ||
        !character_ || !menu_data_ || !lobby_ || (room_entry && !plaza_)) {
      scene_connection_.Cancel();
    } else {
      // Preparation happens on this connection's thread, with no shared scene
      // or party lock held while the trusted data provider performs I/O.
      InvalidateClanScene();
      const auto observed = InventoryObservationTime();
      auto data = LoadMenuData(observed);
      SyncParty();
      if (invitation_lease_) invitation_lease_.Invalidate();
      auto prepared = std::make_shared<PartyScenePrepared>();
      prepared->character = *character_;
      prepared->envelope = data.scene;
      prepared->member = data.member;
      prepared->equipment = data.equipment;
      if (work->request.kind == PartySceneKind::kMenu)
        prepared->menu_packets = MenuInitialization::Prepare(
            *character_, *account_id_, std::move(data), observed);
      scene_prepared_ = prepared;
      prepared->source = std::move(plaza_);
      prepared->source_room_id = room_.RoomId();
      prepared->source_room = std::move(room_);
      if (!scene_connection_.Prepare(work->id, prepared, practice_clock_())) {
        plaza_ = std::move(prepared->source);
        room_ = std::move(prepared->source_room);
        scene_prepared_.reset();
      }
    }
  }
  auto outcome = scene_connection_.Take(practice_clock_());
  if (!outcome) return {};
  if (!outcome->committed) {
    if (outcome->prepared) {
      plaza_ = std::move(outcome->prepared->source);
      room_ = std::move(outcome->prepared->source_room);
    }
    scene_prepared_.reset();
    return std::move(outcome->refusal);
  }
  // A committed destination queue starts with this participant's full S117.
  // Drop delayed packets for the replaced source before exposing that queue.
  plaza_packets_->Clear();
  room_packets_->Clear();
  if (outcome->room) {
    room_ = std::move(outcome->room);
    room_packets_ = outcome->packets;
    state_ = PlayerSessionState::kInRoom;
  } else {
    plaza_ = std::move(outcome->plaza);
    plaza_packets_ = outcome->packets;
    lobby_.emplace();
    if (plaza_) lobby_->SnapshotPublished();
    state_ = PlayerSessionState::kMenuInitializationSent;
  }
  scene_prepared_.reset();
  return outcome->packets->Drain();
}
}  // namespace bubble_fighter::server::sessions
