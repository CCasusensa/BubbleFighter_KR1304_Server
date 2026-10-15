#include "features/inventory/cash_bundle.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "features/inventory/cash_inventory.h"
#include "features/inventory/item_catalog.h"
#include "features/shop/shop_stock_catalog.h"
#include "features/shop/stock_set_catalog.h"

namespace bubble_fighter::server::features::inventory {
void ValidateCashBundle(const CashBundle& bundle) {
  if (bundle.slot_id == 0 || bundle.slot_id > CashInventory::kMaxSlotId ||
      !shop::StockSetCatalog::Find(bundle.stock_set_id) ||
      bundle.cash_price > 0x7FFFFFFFU || bundle.bonus_lucci > 0x7FFFFFFFU ||
      bundle.purchased_at == 0 ||
      bundle.purchased_at > InventorySlot::kMaxUnixSeconds ||
      bundle.components.size() > 10 ||
      (bundle.components.empty() && bundle.bonus_lucci == 0))
    throw std::invalid_argument("Invalid stored cash bundle.");
  std::set<std::uint32_t> stocks;
  for (const auto& part : bundle.components) {
    const auto* stock = shop::ShopStockCatalog::Find(part.stock_id);
    const auto* item = ItemCatalog::Find(part.item_id);
    // Amount, price, cap and bonus are receipt values, not repriced at claim.
    if (!stock || !item || stock->item_id != part.item_id ||
        item->usage != part.usage || !stocks.insert(part.stock_id).second ||
        part.amount == 0 || part.max_amount == 0 ||
        (part.usage != InventoryUsage::kCount &&
         part.usage != InventoryUsage::kLife &&
         part.usage != InventoryUsage::kDuration) ||
        (part.usage == InventoryUsage::kDuration
             ? (part.amount != 0xFFFFFFFFU && part.amount > 10000)
             : part.amount > InventorySlot::kMaxQuantity))
      throw std::invalid_argument("Invalid stored bundle component.");
  }
}
bool CanRefundCashBundle(const CashBundle& bundle, std::uint64_t now) noexcept {
  return bundle.refundable && now != 0 &&
         now <= InventorySlot::kMaxUnixSeconds && now >= bundle.purchased_at &&
         now - bundle.purchased_at < kCashRefundWindowSeconds;
}
InventoryGrantStatus GrantBundle(CharacterInventory& inventory,
                                 const CashBundle& bundle, std::uint64_t now) {
  for (const auto& part : bundle.components) {
    if (inventory.slots.size() >=
            protocol::InventoryItemRecord::kMaxCollectionEntries &&
        std::none_of(
            inventory.slots.begin(), inventory.slots.end(),
            [&](const auto& slot) { return slot.item_id == part.item_id; }))
      return InventoryGrantStatus::kLimitExceeded;
    const auto result = InventoryGrant::Apply(
        inventory, part.item_id, part.amount, part.max_amount, now);
    if (result != InventoryGrantStatus::kGranted) return result;
  }
  return InventoryGrantStatus::kGranted;
}
}  // namespace bubble_fighter::server::features::inventory
