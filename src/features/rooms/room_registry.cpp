#include "features/rooms/room_registry.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "features/chat/scene_chat.h"
#include "features/practice/practice_session.h"
#include "features/rooms/room_roster.h"
#include "protocol/battle/game_return_to_room_notification.h"
#include "protocol/battle/game_transport_options_notification.h"
#include "protocol/rooms/room_host_changed_notification.h"

namespace bubble_fighter::server::features::rooms {
const RoomOccupant* RoomRecord::FindSlot(std::uint32_t slot) const noexcept {
  const auto occupant = std::find_if(
      occupants.begin(), occupants.end(),
      [&](const RoomOccupant& entry) { return entry.slot == slot; });
  return occupant == occupants.end() ? nullptr : &*occupant;
}

bool RoomRecord::IsSeatClosed(std::uint32_t slot) const noexcept {
  return std::find(closed_seats.begin(), closed_seats.end(), slot) !=
         closed_seats.end();
}

std::size_t RoomRecord::OpenSeatCount() const noexcept {
  // A closed seat is always empty (only empty seats can be closed), so the
  // counts never overlap.
  return slot_count - occupants.size() - closed_seats.size();
}

RoomLease::RoomLease(std::shared_ptr<RoomRegistry> registry,
                     std::uint16_t room_id, std::uint64_t account_id) noexcept
    : registry_(std::move(registry)),
      room_id_(room_id),
      account_id_(account_id) {}

RoomLease::RoomLease(RoomLease&& other) noexcept
    : registry_(std::move(other.registry_)),
      room_id_(std::exchange(other.room_id_, std::uint16_t{})),
      account_id_(std::exchange(other.account_id_, std::uint64_t{})) {}

RoomLease& RoomLease::operator=(RoomLease&& other) noexcept {
  if (this != &other) {
    Release();
    registry_ = std::move(other.registry_);
    room_id_ = std::exchange(other.room_id_, std::uint16_t{});
    account_id_ = std::exchange(other.account_id_, std::uint64_t{});
  }
  return *this;
}

RoomLease::~RoomLease() { Release(); }

std::uint16_t RoomLease::RoomId() const noexcept {
  return registry_ ? room_id_ : 0;
}
RoomLease::operator bool() const noexcept { return registry_ != nullptr; }

void RoomLease::Release() noexcept {
  if (registry_) {
    registry_->Leave(room_id_, account_id_);
  }
  registry_.reset();
  room_id_ = 0;
  account_id_ = 0;
}

bool RoomLease::SetMemberState(const std::array<std::uint32_t, 3>& state) {
  if (!registry_) {
    throw std::logic_error("Room state change without a room.");
  }
  return registry_->SetMemberState(room_id_, account_id_, state);
}

RoomSeatStatus RoomLease::SetSeatClosed(
    const protocol::SeatStateRequest& request) {
  if (!registry_) {
    throw std::logic_error("Seat state change without a room.");
  }
  return registry_->SetSeatClosed(room_id_, account_id_, request);
}

RoomTeamStatus RoomLease::ChangeTeam(
    const protocol::TeamChangeRequest& request) {
  if (!registry_) {
    throw std::logic_error("Team change without a room.");
  }
  return registry_->ChangeTeam(room_id_, account_id_, request);
}

RoomStartStatus RoomLease::Start(const protocol::TcpPacket& packet,
                                 std::size_t minimum_players,
                                 std::uint32_t duration_milliseconds,
                                 std::u16string_view trusted_map_key) {
  if (!registry_) {
    throw std::logic_error("Game start without a room.");
  }
  return registry_->Start(room_id_, account_id_, packet, minimum_players,
                          duration_milliseconds, trusted_map_key);
}

void RoomLease::ProcessGame(const protocol::TcpPacket& packet) {
  if (!registry_) {
    throw std::logic_error("Game message outside a room.");
  }
  registry_->ProcessGame(room_id_, account_id_, packet);
}
void RoomLease::Poll() {
  if (registry_) {
    registry_->Poll(room_id_, account_id_);
  }
}
void RoomLease::Chat(const features::chat::SceneChat::Message& request) {
  if (registry_) {
    registry_->Chat(room_id_, account_id_, request);
  }
}
void RoomLease::RefreshCharacter(const characters::CharacterRecord& character) {
  if (registry_) {
    registry_->RefreshCharacter(room_id_, account_id_, character);
  }
}
bool RoomLease::ChangeInventory(
    const std::function<RoomInventoryChange()>& change) {
  return registry_ && registry_->ChangeInventory(room_id_, account_id_, change);
}
bool RoomRegistry::ChangeInventory(
    std::uint16_t room_id, std::uint64_t account_id,
    const std::function<RoomInventoryChange()>& change) {
  {
    std::lock_guard lock(mutex_);
    auto& room = rooms_.at(room_id);
    if (!FindMemberLocked(room, account_id)) {
      throw std::logic_error("Inventory change by non-member.");
    }
    if (room.record.state != RoomState::kWaiting ||
        !room.inventory_changes.insert(account_id).second) {
      return false;
    }
  }
  // Reserve the waiting state, then release the registry lock for database
  // work. Start cannot bind stale equipment while the change is in flight.
  RoomInventoryChange result;
  try {
    result = change();
  } catch (...) {
    std::lock_guard lock(mutex_);
    const auto found = rooms_.find(room_id);
    if (found != rooms_.end()) {
      found->second.inventory_changes.erase(account_id);
    }
    throw;
  }
  std::lock_guard lock(mutex_);
  auto& room = rooms_.at(room_id);
  room.inventory_changes.erase(account_id);
  auto* member = FindMemberLocked(room, account_id);
  if (!member || result.character.account_id != account_id ||
      result.character.character_id != member->character.character_id ||
      result.equipment.key != result.character.character_id ||
      !result.character.statistics.IsValid()) {
    throw std::invalid_argument("Room inventory identity mismatch.");
  }
  if (member->character == result.character &&
      member->equipment == result.equipment) {
    return true;
  }
  const auto previous = *member;
  if (previous.character.character_name != result.character.character_name) {
    room.invitation_epoch = next_invitation_epoch_++;
  }
  member->character = std::move(result.character);
  member->equipment = std::move(result.equipment);
  member->member
      .booleans_at96 = {};  // Changed loadout requires preparing again.
  protocol::TcpPacket packet;
  try {
    packet = RoomRoster::SlotUpdate(room.record, member->slot).Encode();
  } catch (...) {
    *member = previous;
    throw;
  }
  Broadcast(room, {packet});
  return true;
}
void RoomRegistry::RefreshCharacter(
    std::uint16_t room_id, std::uint64_t account_id,
    const characters::CharacterRecord& character) {
  std::lock_guard lock(mutex_);
  auto& room = rooms_.at(room_id);
  auto* member = FindMemberLocked(room, account_id);
  if (!member || character.account_id != account_id ||
      character.character_id != member->character.character_id ||
      character.character_name != member->character.character_name ||
      character.character_type != member->character.character_type ||
      character.color_index != member->character.color_index ||
      !character.statistics.IsValid() || !character.battle.IsValid()) {
    throw std::invalid_argument("Room commerce identity mismatch.");
  }
  // Shop transactions do not change the equipped loadout or room seat.
  // Keep the owned snapshot in sync so Poll cannot restore old balances.
  member->character.statistics = character.statistics;
  member->character.battle = character.battle;
}
void RoomRegistry::Chat(std::uint16_t room_id, std::uint64_t account_id,
                        const features::chat::SceneChat::Message& request) {
  std::lock_guard lock(mutex_);
  const auto& room = rooms_.at(room_id);
  const auto sender = std::find_if(
      room.record.occupants.begin(), room.record.occupants.end(),
      [&](const auto& member) { return member.account_id == account_id; });
  if (sender == room.record.occupants.end()) {
    throw std::logic_error("Chat from non-member.");
  }
  const auto packet = features::chat::SceneChat::Create(
                          static_cast<std::int16_t>(sender->slot),
                          sender->character.character_name, request)
                          .Encode();
  for (const auto& peer : room.record.occupants) {
    if (request.allows_recipient &&
        !request.allows_recipient(peer.character.character_id)) {
      continue;
    }
    if (request.channel == 0 || peer.slot == sender->slot ||
        ((sender->member.signed_byte_at4 == 1 ||
          sender->member.signed_byte_at4 == 2) &&
         peer.member.signed_byte_at4 == sender->member.signed_byte_at4)) {
      Deliver(room.sinks.at(peer.slot), {packet});
    }
  }
}
std::optional<characters::CharacterRecord> RoomLease::Character() const {
  if (!registry_) {
    return {};
  }
  const auto room = registry_->Find(room_id_);
  if (!room) {
    return {};
  }
  for (const auto& occupant : room->occupants) {
    if (occupant.account_id == account_id_) {
      return occupant.character;
    }
  }
  return {};
}
void RoomLease::ReturnToWaiting() {
  if (!registry_) {
    throw std::logic_error("Game exit outside a room.");
  }
  registry_->ReturnToWaiting(room_id_, account_id_);
}

RoomReservation::RoomReservation(std::shared_ptr<RoomRegistry> registry,
                                 std::uint16_t room_id,
                                 std::uint64_t account_id,
                                 std::uint64_t character_id) noexcept
    : registry_(std::move(registry)),
      room_id_(room_id),
      account_id_(account_id),
      character_id_(character_id) {}

RoomReservation::RoomReservation(RoomReservation&& other) noexcept
    : registry_(std::move(other.registry_)),
      room_id_(std::exchange(other.room_id_, std::uint16_t{})),
      account_id_(std::exchange(other.account_id_, std::uint64_t{})),
      character_id_(std::exchange(other.character_id_, std::uint64_t{})) {}

RoomReservation& RoomReservation::operator=(RoomReservation&& other) noexcept {
  if (this != &other) {
    Abandon();
    registry_ = std::move(other.registry_);
    room_id_ = std::exchange(other.room_id_, std::uint16_t{});
    account_id_ = std::exchange(other.account_id_, std::uint64_t{});
    character_id_ = std::exchange(other.character_id_, std::uint64_t{});
  }
  return *this;
}

RoomReservation::~RoomReservation() { Abandon(); }

std::uint16_t RoomReservation::RoomId() const noexcept { return room_id_; }

RoomLease RoomReservation::Commit(
    RoomRecord record, RoomPacketSink host_sink,
    std::vector<protocol::TcpPacket> host_packets) && {
  std::vector<RoomPublication> deliveries;
  deliveries.push_back(
      {account_id_, std::move(host_sink), std::move(host_packets)});
  auto members =
      std::move(*this).CommitGroup(std::move(record), std::move(deliveries));
  return std::move(members.front());
}
std::vector<RoomLease> RoomReservation::CommitGroup(
    RoomRecord record, std::vector<RoomPublication> deliveries) && {
  if (!registry_) throw std::logic_error("Room reservation was already used.");
  auto members =
      registry_->PublishGroup(room_id_, account_id_, character_id_,
                              std::move(record), std::move(deliveries));
  registry_.reset();
  room_id_ = 0;
  return members;
}

void RoomReservation::Abandon() noexcept {
  if (registry_) {
    registry_->Abandon(room_id_);
  }
  registry_.reset();
}

std::shared_ptr<RoomRegistry> RoomRegistry::Create(
    std::size_t max_rooms, GameClock clock, SeedFactory seed,
    RoomMatchSettler settler, MatchRewardPolicy rules,
    std::shared_ptr<RoomSettlementQueue> settlements) {
  if (max_rooms == 0 || max_rooms > kMaxRoomId) {
    throw std::invalid_argument("Unsupported room registry capacity.");
  }
  if (!clock) {
    clock = RoomGameSession::Clock::now;
  }
  if (!seed) {
    seed = practice::PracticeSession::CreateRandomSeed;
  }
  rules.Validate();
  if (settler && settlements)
    throw std::invalid_argument("Duplicate settlement providers.");
  return std::shared_ptr<RoomRegistry>(
      new RoomRegistry(max_rooms, std::move(clock), std::move(seed),
                       std::move(settler), rules, std::move(settlements)));
}

RoomRegistry::RoomRegistry(std::size_t max_rooms, GameClock clock,
                           SeedFactory seed, RoomMatchSettler settler,
                           MatchRewardPolicy rules,
                           std::shared_ptr<RoomSettlementQueue> settlements)
    : max_rooms_(max_rooms),
      clock_(std::move(clock)),
      seed_(std::move(seed)),
      settlements_(
          settlements ? std::move(settlements)
          : settler ? std::make_shared<RoomSettlementQueue>(std::move(settler))
                    : nullptr),
      rules_(rules) {}

bool RoomRegistry::IsUsedLocked(std::uint16_t room_id) const {
  return rooms_.contains(room_id) || pending_.contains(room_id);
}

RoomOccupant* RoomRegistry::FindMemberLocked(Room& room,
                                             std::uint64_t account_id) {
  const auto occupant =
      std::find_if(room.record.occupants.begin(), room.record.occupants.end(),
                   [&](const RoomOccupant& entry) {
                     return entry.account_id == account_id;
                   });
  return occupant == room.record.occupants.end() ? nullptr : &*occupant;
}

void RoomRegistry::Deliver(const RoomPacketSink& sink,
                           std::vector<protocol::TcpPacket> packets) noexcept {
  if (!sink || packets.empty()) {
    return;
  }
  try {
    sink(std::move(packets));
  } catch (...) {
  }
}

void RoomRegistry::Broadcast(
    const Room& room, const std::vector<protocol::TcpPacket>& packets,
    std::optional<std::uint32_t> except_slot) noexcept {
  for (const auto& [slot, sink] : room.sinks) {
    if (slot == except_slot) {
      continue;
    }
    try {
      Deliver(sink, packets);
    } catch (...) {
    }
  }
}

RoomReserveResult RoomRegistry::Reserve(std::uint64_t account_id,
                                        std::uint64_t character_id) {
  return ReserveGroup({{account_id, character_id}});
}

void RoomRegistry::Abandon(std::uint16_t room_id) noexcept {
  std::lock_guard lock(mutex_);
  const auto pending = pending_.find(room_id);
  if (pending == pending_.end()) {
    return;
  }
  for (const auto& owner : pending->second) {
    by_account_.erase(owner.account_id);
    by_character_.erase(owner.character_id);
  }
  pending_.erase(pending);
}

RoomJoinResult RoomRegistry::Join(
    std::uint16_t room_id, const std::u16string& password, RoomOccupant joiner,
    RoomPacketSink sink, std::vector<protocol::TcpPacket> joiner_prefix) {
  return JoinChecked(room_id, password, std::move(joiner), std::move(sink),
                     std::move(joiner_prefix), nullptr);
}

std::optional<RoomInvitationTarget> RoomRegistry::InvitationTarget(
    std::uint64_t inviter_character_id) const {
  std::lock_guard lock(mutex_);
  const auto location = by_character_.find(inviter_character_id);
  if (location == by_character_.end()) return {};
  const auto entry = rooms_.find(location->second);
  if (entry == rooms_.end()) return {};
  const auto& room = entry->second;
  for (const auto& member : room.record.occupants) {
    if (member.character.character_id == inviter_character_id) {
      return RoomInvitationTarget{location->second, room.invitation_epoch,
                                  member.account_id,
                                  member.character.character_name};
    }
  }
  return {};
}

RoomJoinResult RoomRegistry::JoinInvited(const RoomInvitationTarget& invitation,
                                         RoomOccupant joiner,
                                         RoomPacketSink sink) {
  return JoinChecked(invitation.room_id, {}, std::move(joiner), std::move(sink),
                     {}, &invitation);
}

RoomJoinResult RoomRegistry::JoinChecked(
    std::uint16_t room_id, const std::u16string& password, RoomOccupant joiner,
    RoomPacketSink sink, std::vector<protocol::TcpPacket> joiner_prefix,
    const RoomInvitationTarget* invitation) {
  std::vector<RoomAdmissionEntry> entries;
  entries.push_back(
      {std::move(joiner), std::move(sink), std::move(joiner_prefix)});
  auto result =
      JoinGroupChecked(room_id, password, std::move(entries), invitation);
  RoomJoinResult single;
  single.status = result.status;
  if (!result.members.empty()) single.room = std::move(result.members.front());
  return single;
}

void RoomRegistry::Leave(std::uint16_t room_id,
                         std::uint64_t account_id) noexcept {
  std::lock_guard lock(mutex_);
  const auto entry = rooms_.find(room_id);
  if (entry == rooms_.end()) {
    return;
  }
  auto& room = entry->second;
  auto& record = room.record;
  const auto* member = FindMemberLocked(room, account_id);
  if (!member) {
    return;
  }
  const auto slot = member->slot;
  room.invitation_epoch = next_invitation_epoch_++;
  by_account_.erase(account_id);
  by_character_.erase(member->character.character_id);
  std::erase_if(record.occupants, [&](const RoomOccupant& occupant) {
    return occupant.slot == slot;
  });
  room.sinks.erase(slot);
  if (record.occupants.empty()) {
    rooms_.erase(entry);
    return;
  }
  std::vector<protocol::TcpPacket> packets;
  try {
    packets.push_back(RoomRoster::MemberLeft(record, slot).Encode());
    if (record.host_slot == slot) {
      // Local policy: the lowest remaining slot becomes host. S502
      // clears that slot's ready byte (0x009DA3C0); mirror it.
      auto& host = record.occupants.front();
      record.host_slot = host.slot;
      host.member.booleans_at96[0] = 0;
      packets.push_back(
          protocol::RoomHostChangedNotification{host.slot}.Encode());
    }
  } catch (...) {
    // Out of memory while encoding: members keep a stale view, but the
    // departed member is gone from the server state either way.
    packets.clear();
  }
  Broadcast(room, packets);
  if (room.game) {
    // Local pre-match integration policy: departures cancel the shared
    // match, never strand the remaining players at a loading barrier.
    try {
      ResetGameLocked(room);
    } catch (...) {
      room.game.reset();
      record.state = RoomState::kWaiting;
    }
  }
}

bool RoomRegistry::SetMemberState(std::uint16_t room_id,
                                  std::uint64_t account_id,
                                  const std::array<std::uint32_t, 3>& state) {
  std::lock_guard lock(mutex_);
  const auto entry = rooms_.find(room_id);
  if (entry == rooms_.end()) {
    throw std::logic_error("Room state change for a closed room.");
  }
  auto& room = entry->second;
  auto* member = FindMemberLocked(room, account_id);
  if (!member) {
    throw std::logic_error("Room state change by a non-member.");
  }
  if (room.record.state != RoomState::kWaiting) {
    return false;
  }
  const auto previous = member->member.booleans_at96;
  member->member.booleans_at96 = state;
  std::vector<protocol::TcpPacket> packets;
  try {
    packets.push_back(
        RoomRoster::SlotUpdate(room.record, member->slot).Encode());
  } catch (...) {
    member->member.booleans_at96 = previous;
    throw;
  }
  Broadcast(room, packets);
  return true;
}

RoomSeatStatus RoomRegistry::SetSeatClosed(
    std::uint16_t room_id, std::uint64_t account_id,
    const protocol::SeatStateRequest& request) {
  std::lock_guard lock(mutex_);
  const auto entry = rooms_.find(room_id);
  if (entry == rooms_.end()) {
    throw std::logic_error("Seat state change for a closed room.");
  }
  auto& room = entry->second;
  auto& record = room.record;
  const auto* member = FindMemberLocked(room, account_id);
  if (!member) {
    throw std::logic_error("Seat state change by a non-member.");
  }
  // 0x00980300 and 0x0096A730 case 17 both test 0x009EBF60, which compares
  // the local slot with the room's host slot: only the host closes seats.
  if (member->slot != record.host_slot) {
    return RoomSeatStatus::kNotHost;
  }
  if (record.state != RoomState::kWaiting) {
    return RoomSeatStatus::kNotWaiting;
  }
  if (request.slot >= record.slot_count) {
    return RoomSeatStatus::kNoSuchSeat;
  }
  if (record.FindSlot(request.slot)) {
    return RoomSeatStatus::kOccupied;
  }
  if (record.IsSeatClosed(request.slot) == (request.closed != 0)) {
    // The seat already holds the requested state: the members' view is
    // current, so nothing is broadcast.
    return RoomSeatStatus::kUnchanged;
  }
  auto previous = record.closed_seats;
  if (request.closed != 0) {
    record.closed_seats.insert(
        std::lower_bound(record.closed_seats.begin(), record.closed_seats.end(),
                         request.slot),
        request.slot);
  } else {
    std::erase(record.closed_seats, request.slot);
  }
  std::vector<protocol::TcpPacket> packets;
  try {
    packets.push_back(RoomRoster::SeatState(record, request.slot).Encode());
  } catch (...) {
    record.closed_seats = std::move(previous);
    throw;
  }
  Broadcast(room, packets);
  return RoomSeatStatus::kChanged;
}

RoomTeamStatus RoomRegistry::ChangeTeam(
    std::uint16_t room_id, std::uint64_t account_id,
    const protocol::TeamChangeRequest& request) {
  std::lock_guard lock(mutex_);
  const auto entry = rooms_.find(room_id);
  if (entry == rooms_.end()) {
    throw std::logic_error("Team change for a closed room.");
  }
  auto& room = entry->second;
  auto* member = FindMemberLocked(room, account_id);
  if (!member) {
    throw std::logic_error("Team change by a non-member.");
  }
  if (room.record.state != RoomState::kWaiting) {
    return RoomTeamStatus::kNotWaiting;
  }
  const auto current = member->member.signed_byte_at4;
  std::optional<std::uint32_t> team;
  if (request.value_at0) {
    // 0x004B3AC0 and 0x004A1EB0 name a slot: adopt the occupant's team when
    // it is another member's and differs. The sender's own slot, an empty
    // seat and a member that already matches all fall through to the
    // button's own meaning below.
    const auto* target = room.record.FindSlot(*request.value_at0);
    if (target && target->slot != member->slot &&
        target->member.signed_byte_at4 != current &&
        target->member.signed_byte_at4 >= 1 &&
        target->member.signed_byte_at4 <= 2) {
      team = target->member.signed_byte_at4;
    }
  }
  // 0x00ADEF30 sends no value at all: the room stage draws the button of
  // the opposite team, so the request means "move me over". A two-team room
  // makes that the same decision for every other form too.
  const auto requested = static_cast<std::uint8_t>(
      team ? *team : (current == 1 ? 2 : 1));
  const auto previous = member->member.signed_byte_at4;
  member->member.signed_byte_at4 = requested;
  std::vector<protocol::TcpPacket> packets;
  try {
    packets.push_back(
        RoomRoster::SlotUpdate(room.record, member->slot).Encode());
  } catch (...) {
    member->member.signed_byte_at4 = previous;
    throw;
  }
  // Always broadcast, even when the team did not change: the client's own
  // button state follows this snapshot.
  Broadcast(room, packets);
  return RoomTeamStatus::kChanged;
}

RoomStartStatus RoomRegistry::Start(std::uint16_t room_id,
                                    std::uint64_t account_id,
                                    const protocol::TcpPacket& packet,
                                    std::size_t minimum_players,
                                    std::uint32_t duration_milliseconds,
                                    std::u16string_view trusted_map_key) {
  std::lock_guard lock(mutex_);
  const auto entry = rooms_.find(room_id);
  if (entry == rooms_.end()) {
    throw std::logic_error("Game start for a closed room.");
  }
  auto& room = entry->second;
  auto& record = room.record;
  const auto* member = FindMemberLocked(room, account_id);
  if (!member) {
    throw std::logic_error("Game start by a non-member.");
  }
  if (member->slot != record.host_slot) {
    return RoomStartStatus::kNotHost;
  }
  if (record.state != RoomState::kWaiting) {
    return RoomStartStatus::kNotWaiting;
  }
  if (!room.inventory_changes.empty()) {
    return RoomStartStatus::kMembersNotReady;
  }
  if (record.occupants.size() < minimum_players) {
    return RoomStartStatus::kNotEnoughPlayers;
  }
  // 0x0096DD20 checks the same before sending: every occupied slot
  // other than the host has its ready byte (+96) set.
  if (std::any_of(record.occupants.begin(), record.occupants.end(),
                  [&](const RoomOccupant& occupant) {
                    return occupant.slot != record.host_slot &&
                           occupant.member.booleans_at96[0] == 0;
                  })) {
    return RoomStartStatus::kMembersNotReady;
  }
  const auto& selection = record.settings.configuration.texts_at44[0];
  if (!selection.empty() && selection != u"Random" &&
      selection != trusted_map_key) {
    return RoomStartStatus::kUnsupportedMap;
  }
  std::vector<std::uint32_t> slots;
  for (const auto& occupant : record.occupants) {
    slots.push_back(occupant.slot);
  }
  RoomGameSession game(slots, clock_(), seed_(), duration_milliseconds);
  game.BindMap(trusted_map_key);
  for (const auto& occupant : record.occupants) {
    game.BindEquipment(occupant.slot, occupant.equipment.groups[0]);
    game.BindCharacter(occupant.slot, occupant.character.character_type);
    game.BindTeam(occupant.slot,
                  static_cast<std::uint8_t>(occupant.member.signed_byte_at4));
  }
  std::vector<protocol::TcpPacket> packets{
      protocol::GameTransportOptionsNotification{0, 0, 0}.Encode(), packet};
  const auto match_id =
      settlements_ ? CreateRoomMatchId() : std::array<std::uint8_t, 16>{};
  if (settlements_) {
    game.RequireSettlement();
  }
  room.game.emplace(std::move(game));
  room.finished_game.reset();
  room.match_id = match_id;
  room.duration_milliseconds = duration_milliseconds;
  room.pending_settlement.reset();
  room.settlement_complete = room.settlement_delayed = false;
  room.retry_settlement_at = {};
  record.state = RoomState::kLoading;
  room.invitation_epoch = next_invitation_epoch_++;
  Broadcast(room, packets);
  return RoomStartStatus::kStarted;
}

void RoomRegistry::DeliverGameLocked(
    const Room& room, const std::vector<RoomGameDelivery>& deliveries) {
  for (const auto& delivery : deliveries) {
    if (delivery.only_slot) {
      Deliver(room.sinks.at(*delivery.only_slot), {delivery.packet});
    } else {
      Broadcast(room, {delivery.packet}, delivery.except_slot);
    }
  }
}

void RoomRegistry::ResetGameLocked(Room& room, bool return_notified) {
  auto waiting = room.record;
  waiting.state = RoomState::kWaiting;
  for (auto& occupant : waiting.occupants) {
    occupant.member.booleans_at96 = {};
  }
  // S620 must reach the old context before S117 replaces it. Normal
  // result presentation already delivered it; cancellation and loading
  // timeout need the same cleanup (RoomExit: 9EA690 / 9E1D70).
  std::vector<std::pair<std::uint32_t, std::vector<protocol::TcpPacket>>>
      packets;
  for (const auto& occupant : waiting.occupants) {
    std::vector<protocol::TcpPacket> batch;
    if (!return_notified) {
      batch.push_back(protocol::GameReturnToRoomNotification{}.Encode());
    }
    batch.push_back(RoomRoster::Snapshot(waiting, occupant.slot).Encode());
    packets.emplace_back(occupant.slot, std::move(batch));
  }
  room.record = std::move(waiting);
  room.game->Finish();
  room.finished_game = std::move(room.game);
  room.game.reset();
  room.pending_settlement.reset();
  for (auto& [slot, batch] : packets) {
    Deliver(room.sinks.at(slot), std::move(batch));
  }
}

void RoomRegistry::PollLocked(Room& room, RoomGameSession::Time now) {
  if (!room.game) {
    return;
  }
  const auto deliveries = room.game->Poll(now);
  const bool return_notified =
      std::any_of(deliveries.begin(), deliveries.end(),
                  [](const RoomGameDelivery& delivery) {
                    return delivery.packet.message_id ==
                           protocol::GameReturnToRoomNotification::kMessageId;
                  });
  DeliverGameLocked(room, deliveries);
  if (settlements_ && room.game->Phase() == RoomGamePhase::kResult &&
      !room.settlement_complete) {
    SettleLocked(room, now);
  }
  if (room.game->Phase() == RoomGamePhase::kReturning) {
    ResetGameLocked(room, return_notified);
  } else if (room.game->Phase() == RoomGamePhase::kPlaying) {
    room.record.state = RoomState::kPlaying;
  }
}

void RoomRegistry::SettleLocked(Room& room, RoomGameSession::Time now) {
  if (now < room.retry_settlement_at) {
    return;
  }
  try {
    if (!room.pending_settlement) {
      RoomMatchRequest request;
      // V3 has a fixed ordinary-mode counter policy. Future modes must opt
      // into their own evidenced policy; old recovered requests stay v1/v2.
      if (room.record.settings.configuration.short_at4 == 1)
        request.format_version = 3;
      request.match_id = room.match_id;
      request.rules = rules_;
      request.duration_milliseconds = room.duration_milliseconds;
      request.team_scores = room.game->FinalTeamScores();
      for (const auto& occupant : room.record.occupants) {
        request.members.push_back(
            {occupant.slot, occupant.account_id,
             occupant.character.character_id,
             static_cast<std::uint8_t>(occupant.member.signed_byte_at4),
             room.game->FinalScores(occupant.slot)});
      }
      request.Validate();
      room.pending_settlement = settlements_->Enqueue(std::move(request));
    }
    const auto& request = room.pending_settlement->kRequest;
    settlements_->TrySettle(room.pending_settlement, now);
    const auto completed = room.pending_settlement->Result();
    if (!completed) {
      return;
    }
    const auto& result = *completed;
    auto scores = result.Scores(request).Encode();
    auto profiles = result.Profiles(request);
    auto updated = room.record;
    for (const auto& reward : result.rewards) {
      auto* member = FindMemberLocked(room, reward.character.account_id);
      if (!member || member->slot != reward.slot) {
        throw std::logic_error("Settlement roster changed.");
      }
      for (auto& occupant : updated.occupants) {
        if (occupant.slot == reward.slot) {
          // Receipt identity/name belongs to the historical match. The live
          // connection refreshes its current statistics before delivering S105.
          // A replay must never replace its current name/model with old data.
          occupant.character.statistics = reward.character.statistics;
        }
      }
    }
    room.game->ConfirmSettlement(now);
    room.record = std::move(updated);
    room.settlement_complete = true;
    Broadcast(room, {std::move(scores)});
    for (auto& [slot, packet] : profiles) {
      Deliver(room.sinks.at(slot), {std::move(packet)});
    }
    if (room.settlement_delayed) {
      std::cerr << "Local match settlement recovered.\n";
    }
  } catch (const std::exception&) {
    // Keep frozen participants/scores and the same 128-bit identity.
    // Never display uncommitted rewards or restart the match on failure.
    if (!room.settlement_delayed) {
      std::cerr << "Local match settlement delayed; retrying in two seconds.\n";
    }
    room.settlement_delayed = true;
    room.retry_settlement_at = now + std::chrono::seconds{2};
  }
}

void RoomRegistry::RetrySettlements() {
  if (settlements_) {
    settlements_->ProcessPending(clock_());
  }
}

void RoomRegistry::RecoverSettlement(RoomMatchRequest request) {
  if (!settlements_) {
    throw std::logic_error("No settlement service.");
  }
  settlements_->Enqueue(std::move(request));
}

std::size_t RoomRegistry::PendingSettlementCount() const {
  return settlements_ ? settlements_->PendingCount() : 0;
}

void RoomRegistry::Poll(std::uint16_t room_id, std::uint64_t account_id) {
  std::lock_guard lock(mutex_);
  auto& room = rooms_.at(room_id);
  if (!FindMemberLocked(room, account_id)) {
    throw std::logic_error("Polling room by a non-member.");
  }
  PollLocked(room, clock_());
}

void RoomRegistry::ProcessGame(std::uint16_t room_id, std::uint64_t account_id,
                               const protocol::TcpPacket& packet) {
  std::lock_guard lock(mutex_);
  auto& room = rooms_.at(room_id);
  const auto* member = FindMemberLocked(room, account_id);
  if (!member) {
    throw std::runtime_error("Game packet outside an owned room.");
  }
  const auto slot = member->slot;
  const auto now = clock_();
  PollLocked(room, now);
  if (!room.game) {
    if (!room.finished_game) {
      throw std::runtime_error("Game packet before an owned match.");
    }
    // E090 live return crossed an in-flight movement. Validate against
    // the finished participants and discard without replay or reset.
    room.finished_game->Process(slot, packet, now);
    return;
  }
  DeliverGameLocked(room, room.game->Process(slot, packet, now));
}

void RoomRegistry::ReturnToWaiting(std::uint16_t room_id,
                                   std::uint64_t account_id) {
  std::lock_guard lock(mutex_);
  auto& room = rooms_.at(room_id);
  if (!FindMemberLocked(room, account_id) ||
      (!room.game && !room.finished_game)) {
    throw std::runtime_error("Game exit outside an owned match.");
  }
  if (room.game) {
    ResetGameLocked(room);
  }
}

RoomListPage RoomRegistry::List(std::size_t page, std::size_t page_size) const {
  if (page_size == 0) {
    throw std::invalid_argument("Room list page size is zero.");
  }
  std::lock_guard lock(mutex_);
  RoomListPage result;
  if (page > rooms_.size() / page_size) {
    return result;
  }
  auto room = rooms_.begin();
  std::advance(room, static_cast<std::ptrdiff_t>(page * page_size));
  for (; room != rooms_.end() && result.rooms.size() < page_size; ++room) {
    result.rooms.push_back(RoomRoster::Describe(room->second.record));
  }
  result.has_next_page = room != rooms_.end();
  return result;
}

std::optional<RoomRecord> RoomRegistry::Find(std::uint16_t room_id) const {
  std::lock_guard lock(mutex_);
  const auto room = rooms_.find(room_id);
  if (room == rooms_.end()) {
    return std::nullopt;
  }
  return room->second.record;
}

std::optional<std::uint16_t> RoomRegistry::FindByCharacter(
    std::uint64_t character_id) const {
  std::lock_guard lock(mutex_);
  const auto entry = by_character_.find(character_id);
  if (entry == by_character_.end() || !rooms_.contains(entry->second)) {
    return std::nullopt;
  }
  return entry->second;
}

std::size_t RoomRegistry::Count() const {
  std::lock_guard lock(mutex_);
  return rooms_.size();
}
}  // namespace bubble_fighter::server::features::rooms
