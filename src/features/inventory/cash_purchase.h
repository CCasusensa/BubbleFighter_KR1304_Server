#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CASH_PURCHASE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CASH_PURCHASE_H_

#include "features/inventory/inventory_slot.h"

namespace bubble_fighter::server::features::inventory {
struct CashInventory;
struct CashPurchase {
  std::uint32_t purchase_id = 0;
  std::uint32_t stock_id = 0;
  std::uint32_t item_id = 0;
  InventoryUsage usage = InventoryUsage::kCount;
  std::uint32_t amount = 0;
  std::uint32_t max_amount = 0;
  std::uint32_t cash_price = 0;
  std::uint64_t purchased_at_unix_seconds = 0;
  std::uint32_t slot_count = 0;
  bool claimed = false;
  bool operator==(const CashPurchase&) const = default;
};

// Project policy: complete unclaimed purchases, strictly before seven days.
inline constexpr std::uint64_t kCashRefundWindowSeconds = 7 * 24 * 60 * 60;
void ValidateCashPurchases(const CashInventory& inventory);
bool CanRefundCashPurchase(const CashPurchase& purchase,
                           std::uint64_t observed_at_unix_seconds) noexcept;
}  // namespace bubble_fighter::server::features::inventory

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CASH_PURCHASE_H_
