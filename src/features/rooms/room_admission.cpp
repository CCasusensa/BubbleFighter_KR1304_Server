#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "features/rooms/room_creation.h"
#include "features/rooms/room_registry.h"
#include "features/rooms/room_roster.h"

namespace bubble_fighter::server::features::rooms {
namespace {
void ValidateIdentities(const std::vector<RoomIdentity>& identities) {
  if (identities.empty() ||
      identities.size() > RoomCreation::kOrdinaryRoomSlots) {
    throw std::invalid_argument("Unsupported room admission group.");
  }
  std::set<std::uint64_t> accounts, characters;
  for (const auto& identity : identities) {
    if (identity.account_id == 0 || identity.character_id == 0 ||
        identity.character_id > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("Room owner identity is missing.");
    }
    if (!accounts.insert(identity.account_id).second ||
        !characters.insert(identity.character_id).second) {
      throw std::invalid_argument("Duplicate room admission identity.");
    }
  }
}
void ValidateOccupant(const RoomOccupant& occupant,
                      const RoomPacketSink& sink) {
  if (occupant.account_id == 0 ||
      occupant.character.account_id != occupant.account_id ||
      occupant.character.character_id == 0 ||
      occupant.equipment.key != occupant.character.character_id || !sink) {
    throw std::invalid_argument(
        "Room joiner does not belong to the authenticated account.");
  }
}
}  // namespace

RoomReserveResult RoomRegistry::ReserveGroup(
    const std::vector<RoomIdentity>& owners) {
  ValidateIdentities(owners);
  auto self = shared_from_this();
  std::lock_guard lock(mutex_);
  for (const auto& owner : owners) {
    if (by_account_.contains(owner.account_id) ||
        by_character_.contains(owner.character_id)) {
      return {RoomReserveStatus::kAlreadyInRoom, {}};
    }
  }
  if (rooms_.size() + pending_.size() >= max_rooms_) {
    return {RoomReserveStatus::kRegistryFull, {}};
  }
  auto room_id = next_room_id_;
  while (IsUsedLocked(room_id)) {
    room_id =
        room_id == kMaxRoomId ? 1 : static_cast<std::uint16_t>(room_id + 1);
  }
  pending_.emplace(room_id, owners);
  try {
    for (const auto& owner : owners) {
      by_account_.emplace(owner.account_id, room_id);
      by_character_.emplace(owner.character_id, room_id);
    }
  } catch (...) {
    for (const auto& owner : owners) {
      by_account_.erase(owner.account_id);
      by_character_.erase(owner.character_id);
    }
    pending_.erase(room_id);
    throw;
  }
  next_room_id_ =
      room_id == kMaxRoomId ? 1 : static_cast<std::uint16_t>(room_id + 1);
  RoomReserveResult result;
  result.status = RoomReserveStatus::kReserved;
  result.reservation.emplace(RoomReservation(std::move(self), room_id,
                                             owners.front().account_id,
                                             owners.front().character_id));
  return result;
}

std::vector<RoomLease> RoomRegistry::PublishGroup(
    std::uint16_t room_id, std::uint64_t account_id, std::uint64_t character_id,
    RoomRecord record, std::vector<RoomPublication> deliveries) {
  auto self = shared_from_this();
  std::lock_guard lock(mutex_);
  const auto pending = pending_.find(room_id);
  if (pending == pending_.end() ||
      pending->second.front() != RoomIdentity{account_id, character_id}) {
    throw std::logic_error("Room publication does not match its reservation.");
  }
  const auto& owners = pending->second;
  const auto* host = record.FindSlot(record.host_slot);
  if (record.room_id != room_id || record.slot_count == 0 ||
      record.slot_count > RoomCreation::kOrdinaryRoomSlots ||
      record.host_slot >= record.slot_count ||
      record.state != RoomState::kWaiting ||
      record.occupants.size() != owners.size() ||
      deliveries.size() != owners.size() || !host ||
      host->account_id != account_id ||
      host->character.character_id != character_id) {
    throw std::invalid_argument(
        "Room record does not describe the reserved host.");
  }
  Room room;
  std::set<std::uint64_t> seen;
  std::uint32_t previous_slot = 0;
  bool first = true;
  for (const auto& occupant : record.occupants) {
    const RoomIdentity identity{occupant.account_id,
                                occupant.character.character_id};
    const auto delivery = std::find_if(
        deliveries.begin(), deliveries.end(), [&](const auto& value) {
          return value.account_id == occupant.account_id;
        });
    if (occupant.slot >= record.slot_count ||
        (!first && occupant.slot <= previous_slot) ||
        std::find(owners.begin(), owners.end(), identity) == owners.end() ||
        !seen.insert(occupant.account_id).second ||
        delivery == deliveries.end()) {
      throw std::invalid_argument(
          "Room record does not describe the reserved host.");
    }
    ValidateOccupant(occupant, delivery->sink);
    room.sinks.emplace(occupant.slot, delivery->sink);
    previous_slot = occupant.slot;
    first = false;
  }
  // Check every slot before Snapshot sizes its positional vector from the
  // final slot. Malformed trusted input must not trigger oversized allocation.
  // Legacy Commit may deliberately supply no initial packets.
  for (const auto& occupant : record.occupants) {
    RoomRoster::Snapshot(record, occupant.slot).Encode();
  }
  std::vector<RoomLease> members;
  members.reserve(owners.size());
  room.record = std::move(record);
  room.invitation_epoch = next_invitation_epoch_++;
  rooms_.emplace(room_id, std::move(room));
  // All allocation and encoding completed. Constructing leases is noexcept;
  // no lease can unwind under mutex_ and recursively attempt Leave.
  for (const auto& owner : owners) {
    members.push_back(RoomLease(self, room_id, owner.account_id));
  }
  pending_.erase(pending);
  for (auto& delivery : deliveries) {
    Deliver(delivery.sink, std::move(delivery.packets));
  }
  return members;
}

RoomGroupJoinResult RoomRegistry::JoinGroup(
    std::uint16_t room_id, const std::u16string& password,
    std::vector<RoomAdmissionEntry> entries) {
  return JoinGroupChecked(room_id, password, std::move(entries), nullptr);
}
RoomGroupJoinResult RoomRegistry::JoinGroupInvited(
    const RoomInvitationTarget& invitation,
    std::vector<RoomAdmissionEntry> entries) {
  return JoinGroupChecked(invitation.room_id, {}, std::move(entries),
                          &invitation);
}
RoomGroupJoinResult RoomRegistry::JoinGroupChecked(
    std::uint16_t room_id, const std::u16string& password,
    std::vector<RoomAdmissionEntry> entries,
    const RoomInvitationTarget* invitation) {
  std::vector<RoomIdentity> identities;
  for (const auto& entry : entries) {
    ValidateOccupant(entry.occupant, entry.sink);
    identities.push_back(
        {entry.occupant.account_id, entry.occupant.character.character_id});
  }
  ValidateIdentities(identities);
  auto self = shared_from_this();
  std::lock_guard lock(mutex_);
  for (const auto& identity : identities) {
    if (by_account_.contains(identity.account_id) ||
        by_character_.contains(identity.character_id)) {
      throw std::logic_error("Room joiner is already in a room.");
    }
  }
  RoomGroupJoinResult result;
  const auto found = rooms_.find(room_id);
  if (found == rooms_.end()) return result;
  auto& room = found->second;
  // Preserve the established invitation refusal while a game is in progress;
  // the stale target remains invalid after returning to the waiting room.
  if (room.record.state != RoomState::kWaiting) {
    result.status = RoomJoinStatus::kPlaying;
    return result;
  }
  if (invitation) {
    const auto* inviter =
        FindMemberLocked(room, invitation->inviter_account_id);
    if (invitation->epoch != room.invitation_epoch || !inviter ||
        invitation->inviter_name != inviter->character.character_name)
      return result;
  }
  if (!invitation && password != room.record.password) {
    result.status = RoomJoinStatus::kWrongPassword;
    return result;
  }
  // C509: closed seats keep no joiner, so they are not capacity either.
  if (entries.size() > room.record.OpenSeatCount()) {
    result.status = RoomJoinStatus::kFull;
    return result;
  }
  auto candidate = room.record;
  auto sinks = room.sinks;
  for (auto& entry : entries) {
    auto& member = entry.occupant;
    member.slot = 0;
    while (candidate.FindSlot(member.slot) ||
           candidate.IsSeatClosed(member.slot))
      ++member.slot;
    const auto first_team = std::count_if(
        candidate.occupants.begin(), candidate.occupants.end(),
        [](const auto& value) { return value.member.signed_byte_at4 == 1; });
    const auto second_team = std::count_if(
        candidate.occupants.begin(), candidate.occupants.end(),
        [](const auto& value) { return value.member.signed_byte_at4 == 2; });
    member.member.signed_byte_at4 = first_team <= second_team ? 1 : 2;
    member.member.booleans_at96 = {};
    const auto position = std::find_if(
        candidate.occupants.begin(), candidate.occupants.end(),
        [&](const auto& value) { return value.slot > member.slot; });
    candidate.occupants.insert(position, member);
    sinks.emplace(member.slot, entry.sink);
  }
  std::vector<protocol::TcpPacket> updates;
  for (auto& entry : entries) {
    entry.prefix.push_back(
        RoomRoster::Snapshot(candidate, entry.occupant.slot).Encode());
    updates.push_back(
        RoomRoster::SlotUpdate(candidate, entry.occupant.slot).Encode());
  }
  std::vector<RoomPublication> deliveries;
  for (const auto& occupant : room.record.occupants) {
    deliveries.push_back(
        {occupant.account_id, room.sinks.at(occupant.slot), updates});
  }
  for (auto& entry : entries) {
    deliveries.push_back({entry.occupant.account_id, std::move(entry.sink),
                          std::move(entry.prefix)});
  }
  result.members.reserve(entries.size());
  try {
    for (const auto& identity : identities) {
      by_account_.emplace(identity.account_id, room_id);
      by_character_.emplace(identity.character_id, room_id);
    }
  } catch (...) {
    for (const auto& identity : identities) {
      by_account_.erase(identity.account_id);
      by_character_.erase(identity.character_id);
    }
    throw;
  }
  static_assert(std::is_nothrow_move_assignable_v<RoomRecord>);
  static_assert(std::is_nothrow_move_assignable_v<decltype(sinks)>);
  room.record = std::move(candidate);
  room.sinks = std::move(sinks);
  // Joining does not invalidate invitations from existing members.
  for (const auto& identity : identities) {
    result.members.push_back(RoomLease(self, room_id, identity.account_id));
  }
  result.status = RoomJoinStatus::kJoined;
  for (auto& delivery : deliveries)
    Deliver(delivery.sink, std::move(delivery.packets));
  return result;
}
}  // namespace bubble_fighter::server::features::rooms
