#include "features/inventory/cash_purchase.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "features/inventory/cash_inventory.h"

namespace bubble_fighter::server::features::inventory {
void ValidateCashPurchases(const CashInventory& inventory) {
  std::set<std::uint32_t> ids;
  if (inventory.purchases.size() >
      protocol::CashInventoryUpdateNotification::kMaxSlots) {
    throw std::invalid_argument("Oversized cash purchase snapshot.");
  }
  for (const auto& purchase : inventory.purchases) {
    if (purchase.purchase_id == 0 ||
        purchase.purchase_id > CashInventory::kMaxSlotId ||
        !ids.insert(purchase.purchase_id).second || purchase.slot_count == 0 ||
        purchase.slot_count >
            protocol::CashInventoryUpdateNotification::kMaxSlots ||
        purchase.amount == 0 || purchase.max_amount == 0 ||
        purchase.cash_price > 0x7FFFFFFFU ||
        purchase.purchased_at_unix_seconds > InventorySlot::kMaxUnixSeconds) {
      throw std::invalid_argument("Invalid cash purchase receipt.");
    }
    std::uint64_t amount = 0, price = 0;
    std::uint32_t count = 0;
    for (const auto& slot : inventory.slots) {
      if (slot.purchase_id != purchase.purchase_id) continue;
      if (slot.stock_id != purchase.stock_id ||
          slot.item_id != purchase.item_id || slot.usage != purchase.usage ||
          slot.max_amount != purchase.max_amount ||
          slot.purchased_at_unix_seconds !=
              purchase.purchased_at_unix_seconds) {
        throw std::invalid_argument("Cash purchase slot metadata mismatch.");
      }
      ++count;
      amount += slot.amount;
      price += slot.cash_price;
    }
    if (count == 0 || count > purchase.slot_count || amount > purchase.amount ||
        price > purchase.cash_price ||
        (!purchase.claimed &&
         (count != purchase.slot_count || amount != purchase.amount ||
          price != purchase.cash_price))) {
      throw std::invalid_argument("Incomplete unclaimed cash purchase.");
    }
  }
  for (const auto& slot : inventory.slots) {
    if (slot.purchase_id != 0 && !ids.contains(slot.purchase_id)) {
      throw std::invalid_argument("Missing cash purchase receipt.");
    }
  }
}

bool CanRefundCashPurchase(const CashPurchase& purchase,
                           std::uint64_t observed_at_unix_seconds) noexcept {
  return !purchase.claimed && observed_at_unix_seconds != 0 &&
         observed_at_unix_seconds <= InventorySlot::kMaxUnixSeconds &&
         observed_at_unix_seconds >= purchase.purchased_at_unix_seconds &&
         observed_at_unix_seconds - purchase.purchased_at_unix_seconds <
             kCashRefundWindowSeconds;
}
}  // namespace bubble_fighter::server::features::inventory
