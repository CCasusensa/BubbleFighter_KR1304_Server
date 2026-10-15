#include "features/inventory/delivery_capacity.h"

#include <algorithm>
#include <map>
#include <stdexcept>

#include "features/inventory/inventory_validation.h"
#include "features/inventory/item_catalog.h"

namespace bubble_fighter::server::features::inventory {
void AppendCashDelivery(PendingDelivery& delivery, const CashInventory& cash) {
  for (const auto& slot : cash.slots)
    delivery.components.push_back({slot.stock_id, slot.item_id, slot.usage,
                                   slot.amount, slot.max_amount});
  for (const auto& bundle : cash.bundles) {
    delivery.components.insert(delivery.components.end(),
                               bundle.components.begin(),
                               bundle.components.end());
    delivery.bonus_lucci += bundle.bonus_lucci;
  }
}
InventoryGrantStatus CheckDeliveryCapacity(
    const characters::CharacterRecord& character,
    const CharacterInventory& inventory, const PendingDelivery& delivery,
    std::uint64_t now) {
  if (now == 0 || now > InventorySlot::kMaxUnixSeconds ||
      !IsValidInventoryCharacter(character) ||
      !BelongsToCharacter(character, inventory) ||
      !MatchesItemCatalog(inventory))
    throw std::invalid_argument("Invalid pending delivery owner or inventory.");
  (void)inventory.ToRecords(character.account_id, now);
  if (delivery.bonus_lucci >
      characters::CharacterStatistics::kMaxValue - character.statistics.lucci)
    return InventoryGrantStatus::kLimitExceeded;
  struct Group {
    std::uint64_t amount = 0;
    std::uint32_t maximum = 0xFFFFFFFFU;
    std::size_t parts = 0;
    bool permanent = false;
  };
  std::map<std::uint32_t, Group> groups;
  for (const auto& part : delivery.components) {
    const auto* item = ItemCatalog::Find(part.item_id);
    if (!item || item->usage != part.usage || part.amount == 0 ||
        part.max_amount == 0 ||
        (part.usage != InventoryUsage::kCount &&
         part.usage != InventoryUsage::kDuration &&
         part.usage != InventoryUsage::kLife))
      throw std::invalid_argument("Invalid pending delivery component.");
    auto& group = groups[part.item_id];
    ++group.parts;
    group.maximum = (std::min)(group.maximum, part.max_amount);
    if (part.usage == InventoryUsage::kDuration && part.amount == 0xFFFFFFFFU)
      group.permanent = true;
    else {
      group.amount += part.amount;
      if (group.amount > InventorySlot::kMaxQuantity)
        return InventoryGrantStatus::kLimitExceeded;
    }
    // A permanent pending part would prevent another part from being claimed
    // if selected first. No arbitrary warehouse/gift order is imposed.
    if (group.permanent && group.parts != 1)
      return InventoryGrantStatus::kAlreadyOwned;
  }
  auto projected = inventory;
  for (const auto& [id, group] : groups) {
    if (projected.slots.size() >=
            protocol::InventoryItemRecord::kMaxCollectionEntries &&
        std::none_of(projected.slots.begin(), projected.slots.end(),
                     [&](const auto& row) { return row.item_id == id; }))
      return InventoryGrantStatus::kLimitExceeded;
    const auto amount = group.permanent
                            ? 0xFFFFFFFFU
                            : static_cast<std::uint32_t>(group.amount);
    const auto status =
        InventoryGrant::Apply(projected, id, amount, group.maximum, now);
    if (status != InventoryGrantStatus::kGranted) return status;
  }
  return InventoryGrantStatus::kGranted;
}
}  // namespace bubble_fighter::server::features::inventory
