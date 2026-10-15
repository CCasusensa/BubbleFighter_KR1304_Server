#include <algorithm>
#include <type_traits>
#include <utility>

#include "features/rooms/room_registry.h"
#include "features/rooms/room_roster.h"
#include "protocol/rooms/room_host_changed_notification.h"

namespace bubble_fighter::server::features::rooms {
bool RoomRegistry::DepartGroup(const std::vector<RoomDeparture>& members,
                               const std::function<bool()>& admit_destination) {
  if (members.empty() || !admit_destination) return false;
  std::lock_guard lock(mutex_);
  struct Departure {
    RoomRecord remaining;
    std::vector<std::uint32_t> slots;
    std::vector<RoomPublication> deliveries;
  };
  std::map<std::uint16_t, Departure> departures;
  std::set<std::uint64_t> accounts;
  for (const auto& member : members) {
    const auto* lease = member.lease;
    if (!lease || lease->registry_.get() != this ||
        lease->account_id_ != member.identity.account_id ||
        !accounts.insert(member.identity.account_id).second)
      return false;
    const auto found = rooms_.find(lease->room_id_);
    if (found == rooms_.end()) return false;
    auto& room = found->second;
    const auto* occupant = FindMemberLocked(room, lease->account_id_);
    if (!occupant ||
        occupant->character.character_id != member.identity.character_id ||
        room.record.state != RoomState::kWaiting || room.game ||
        !room.inventory_changes.empty())
      return false;
    auto [entry, inserted] = departures.try_emplace(lease->room_id_);
    auto& departure = entry->second;
    if (inserted) departure.remaining = room.record;
    departure.slots.push_back(occupant->slot);
    std::erase_if(departure.remaining.occupants, [&](const auto& other) {
      return other.account_id == lease->account_id_;
    });
  }
  for (auto& [id, departure] : departures) {
    auto& remaining = departure.remaining;
    if (remaining.occupants.empty()) continue;
    std::vector<protocol::TcpPacket> packets;
    std::sort(departure.slots.begin(), departure.slots.end());
    for (const auto slot : departure.slots)
      packets.push_back(RoomRoster::MemberLeft(remaining, slot).Encode());
    if (!remaining.FindSlot(remaining.host_slot)) {
      auto& host = remaining.occupants.front();
      remaining.host_slot = host.slot;
      host.member.booleans_at96[0] = 0;
      packets.push_back(
          protocol::RoomHostChangedNotification{host.slot}.Encode());
    }
    // Copy every recipient's batch before the destination is published.
    for (const auto& occupant : remaining.occupants)
      departure.deliveries.push_back({occupant.account_id,
                                      rooms_.at(id).sinks.at(occupant.slot),
                                      packets});
  }
  if (!admit_destination()) return false;
  static_assert(std::is_nothrow_move_assignable_v<RoomRecord>);
  for (const auto& member : members) {
    by_account_.erase(member.identity.account_id);
    by_character_.erase(member.identity.character_id);
    // Consumed handles cannot later remove a reused account/room identity.
    member.lease->registry_.reset();
    member.lease->room_id_ = 0;
    member.lease->account_id_ = 0;
  }
  for (auto& [id, departure] : departures) {
    auto& room = rooms_.at(id);
    if (departure.remaining.occupants.empty()) {
      rooms_.erase(id);
    } else {
      room.record = std::move(departure.remaining);
      room.invitation_epoch = next_invitation_epoch_++;
      for (const auto slot : departure.slots) room.sinks.erase(slot);
    }
    for (auto& delivery : departure.deliveries)
      Deliver(delivery.sink, std::move(delivery.packets));
  }
  return true;
}
}  // namespace bubble_fighter::server::features::rooms
