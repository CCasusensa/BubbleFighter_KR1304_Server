#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SHOP_BUNDLE_SHOP_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SHOP_BUNDLE_SHOP_H_

#include <optional>

#include "features/shop/cash_refund.h"
#include "features/shop/cash_shop.h"

namespace bubble_fighter::server::features::shop {
// Separate project offer policy: listed standard bundles, positive bounded
// prices and representable ordinary components. Historical dates are metadata.
std::optional<inventory::CashBundle> PrepareBundleOffer(std::uint32_t id);
// The one currency decision for a bundle offer: the published row wins, the
// resource row is the fallback. The slot allocator and the delivery branch
// must both call this: deciding the two separately charged a bundle in one
// currency and filed it in the other.
bool BundleIsCashPriced(std::uint32_t id);
class BundleShop {
 public:
  static CashPurchaseResult Purchase(
      const protocol::ShopPurchaseRequest& request,
      const characters::CharacterRecord& character,
      const inventory::CharacterInventory& inventory,
      inventory::CashInventory pending, std::uint32_t assigned_slot_id,
      std::uint64_t now);
  static CashPickupResult Pickup(
      const protocol::CashInventoryPickupRequest& request,
      const characters::CharacterRecord& character,
      inventory::CharacterInventory inventory, inventory::CashInventory pending,
      std::uint64_t now);
  static CashRefundResult Refund(const protocol::CashRefundRequest& request,
                                 const characters::CharacterRecord& character,
                                 inventory::CashInventory pending,
                                 std::uint64_t now);
};
}  // namespace bubble_fighter::server::features::shop

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SHOP_BUNDLE_SHOP_H_
