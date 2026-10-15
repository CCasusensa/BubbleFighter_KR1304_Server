#include <mutex>
#include <utility>
#include <vector>

#include "features/rooms/room_map_catalog.h"
#include "features/rooms/room_registry.h"
#include "features/rooms/room_roster.h"
#include "protocol/scenes/scene_descriptor_notification.h"

namespace bubble_fighter::server::features::rooms {
bool RoomLease::Configure(const protocol::RoomConfigurationRequest& request) {
  return registry_ && registry_->Configure(room_id_, account_id_, request);
}

bool RoomRegistry::Configure(
    std::uint16_t room_id, std::uint64_t account_id,
    const protocol::RoomConfigurationRequest& request) {
  (void)request.Encode();
  std::lock_guard lock(mutex_);
  auto& room = rooms_.at(room_id);
  const auto* member = FindMemberLocked(room, account_id);
  if (!member || member->slot != room.record.host_slot ||
      room.record.state != RoomState::kWaiting ||
      !room.inventory_changes.empty()) {
    return false;
  }
  auto changed = room.record;
  if (!RoomMapCatalog::ApplySelection(changed.settings.configuration,
                                      request)) {
    return false;
  }
  // Encode the entire update before committing; queue it under the same lock.
  // A changed map invalidates readiness, while ownership and equipment persist.
  std::vector<protocol::TcpPacket> packets{
      protocol::SceneDescriptorNotification{RoomRoster::Describe(changed)}
          .Encode()};
  for (auto& occupant : changed.occupants) {
    if (occupant.member.booleans_at96[0] == 0) continue;
    occupant.member.booleans_at96[0] = 0;
    packets.push_back(RoomRoster::SlotUpdate(changed, occupant.slot).Encode());
  }
  room.record = std::move(changed);
  Broadcast(room, packets);
  return true;
}
}  // namespace bubble_fighter::server::features::rooms
