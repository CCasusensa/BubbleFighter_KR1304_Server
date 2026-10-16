#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_EQUIPMENT_CHANGE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_EQUIPMENT_CHANGE_H_

#include "features/inventory/equipment_view.h"

namespace bubble_fighter::server::features::inventory {
struct EquipmentChangeResult {
  // Character selection rewrites the persisted fighter; the caller owns the
  // decision to store it. Unchanged for ordinary equipment requests.
  characters::CharacterRecord character;
  CharacterInventory inventory;
  std::vector<protocol::TcpPacket> packets;
};

class EquipmentChange {
 public:
  // Pure preparation: validates owned clothing, weapons, character items and
  // articles (pets, titles, function cards), removes conflicting positions and
  // encodes the entire update before any write. A request that names nothing
  // this server models is answered with an empty result - an equip the client
  // cannot complete must never end the session.
  static EquipmentChangeResult Prepare(
      const characters::CharacterRecord& character,
      CharacterInventory inventory, std::uint64_t item_identifier, bool equip,
      std::uint64_t observed_at_unix_seconds);
};
}  // namespace bubble_fighter::server::features::inventory

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_EQUIPMENT_CHANGE_H_
