#ifndef BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_BUNDLE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_BUNDLE_H_

#include <optional>

#include "features/inventory/cash_bundle.h"

namespace bubble_fighter::server::features::gifts {
// Immutable delivery terms, deliberately without purchase/refund ownership.
struct GiftBundle {
  std::uint32_t bonus_lucci = 0;
  std::vector<inventory::BundleComponent> components;
  bool operator==(const GiftBundle&) const = default;
};
std::optional<GiftBundle> PrepareGiftBundle(std::uint32_t stock_set_id);
void ValidateGiftBundle(std::uint32_t stock_set_id, const GiftBundle& bundle);
inventory::InventoryGrantStatus GrantGiftBundle(
    inventory::CharacterInventory& inventory, const GiftBundle& bundle,
    std::uint64_t now);
}  // namespace bubble_fighter::server::features::gifts

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_BUNDLE_H_
