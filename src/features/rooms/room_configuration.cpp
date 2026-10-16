#include <iostream>
#include <mutex>
#include <syncstream>
#include <utility>
#include <vector>

#include "features/rooms/room_map_catalog.h"
#include "features/rooms/room_registry.h"
#include "features/rooms/room_roster.h"
#include "protocol/scenes/scene_descriptor_notification.h"

namespace bubble_fighter::server::features::rooms {
namespace {
// Diagnostic only: the client shows one generic notice for every refused room
// configuration, so the gate that actually failed is logged here. Map tokens
// are ASCII resource keys.
std::string Narrow(std::u16string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const auto unit : text)
    result.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
  return result;
}
}  // namespace

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
    std::osyncstream(std::cout)
        << "Room configure refused: room=" << room_id
        << " account=" << account_id
        << " member=" << (member ? "yes" : "no")
        << " slot=" << (member ? static_cast<int>(member->slot) : -1)
        << " host-slot=" << static_cast<int>(room.record.host_slot)
        << " state=" << static_cast<int>(room.record.state)
        << " pending-inventory=" << room.inventory_changes.size() << std::endl;
    return false;
  }
  auto changed = room.record;
  if (!RoomMapCatalog::ApplySelection(changed.settings.configuration,
                                      request)) {
    std::osyncstream(std::cout)
        << "Room configure refused: room=" << room_id
        << " account=" << account_id << " map=" << Narrow(request.map_key)
        << " dir=" << Narrow(request.map_directory)
        << " difficulty=" << request.values_at76[6]
        << " mode=" << request.values_at76[7] << "/"
        << changed.settings.configuration.short_at4
        << " kind=" << request.values_at76[8] << "/"
        << changed.settings.configuration.values_at20[0]
        << " in-catalog="
        << (RoomMapCatalog::Find(request.map_key) != nullptr ? "yes" : "no")
        << std::endl;
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
