#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_DELIVERY_CAPACITY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_DELIVERY_CAPACITY_H_

#include "features/characters/character_store.h"
#include "features/inventory/cash_inventory.h"

namespace bubble_fighter::server::features::inventory {
// A projection of already authorized pending rewards, never an entitlement.
struct PendingDelivery {
  std::vector<BundleComponent> components;
  std::uint64_t bonus_lucci = 0;
};
void AppendCashDelivery(PendingDelivery& delivery, const CashInventory& cash);
// Project policy: every pending part must remain deliverable in any order.
InventoryGrantStatus CheckDeliveryCapacity(
    const characters::CharacterRecord& character,
    const CharacterInventory& inventory, const PendingDelivery& delivery,
    std::uint64_t now);
}  // namespace bubble_fighter::server::features::inventory

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_DELIVERY_CAPACITY_H_
