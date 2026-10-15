#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CASH_BUNDLE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CASH_BUNDLE_H_

#include "features/inventory/inventory_grant.h"

namespace bubble_fighter::server::features::inventory {
struct BundleComponent {
  std::uint32_t stock_id = 0;
  std::uint32_t item_id = 0;
  InventoryUsage usage = InventoryUsage::kUnknown;
  std::uint32_t amount = 0;
  std::uint32_t max_amount = 0;
  bool operator==(const BundleComponent&) const = default;
};
// One cash warehouse slot and one immutable receipt per complete bundle.
// Completed receipts remain in persistence but are absent from this snapshot.
struct CashBundle {
  std::uint32_t slot_id = 0;
  std::uint32_t stock_set_id = 0;
  std::uint32_t cash_price = 0;
  std::uint32_t bonus_lucci = 0;
  std::uint64_t purchased_at = 0;
  bool refundable = false;
  std::vector<BundleComponent> components;
  bool operator==(const CashBundle&) const = default;
};
void ValidateCashBundle(const CashBundle& bundle);
bool CanRefundCashBundle(const CashBundle& bundle, std::uint64_t now) noexcept;
// The caller owns this projection; failure can leave it partially modified.
InventoryGrantStatus GrantBundle(CharacterInventory& inventory,
                                 const CashBundle& bundle, std::uint64_t now);
}  // namespace bubble_fighter::server::features::inventory

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CASH_BUNDLE_H_
