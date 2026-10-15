#include "features/gifts/gift_bundle.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "features/inventory/item_catalog.h"
#include "features/shop/bundle_shop.h"
#include "features/shop/shop_stock_catalog.h"
#include "features/shop/stock_set_catalog.h"

namespace bubble_fighter::server::features::gifts {
using namespace inventory;
std::optional<GiftBundle> PrepareGiftBundle(std::uint32_t stock_set_id) {
  const auto* definition = shop::StockSetCatalog::Find(stock_set_id);
  auto offer = shop::PrepareBundleOffer(stock_set_id);
  // Native E208D5 rejects Life components before opening the gift dialog.
  if (!definition || !definition->can_present || !offer ||
      std::any_of(
          offer->components.begin(), offer->components.end(),
          [](const auto& part) { return part.usage == InventoryUsage::kLife; }))
    return {};
  return GiftBundle{offer->bonus_lucci, std::move(offer->components)};
}
void ValidateGiftBundle(std::uint32_t stock_set_id, const GiftBundle& bundle) {
  if (!shop::StockSetCatalog::Find(stock_set_id) ||
      bundle.bonus_lucci > 0x7FFFFFFFU || bundle.components.size() > 10 ||
      (bundle.components.empty() && bundle.bonus_lucci == 0))
    throw std::invalid_argument("Invalid stored gift bundle.");
  std::set<std::uint32_t> stocks;
  for (const auto& part : bundle.components) {
    const auto* stock = shop::ShopStockCatalog::Find(part.stock_id);
    const auto* item = ItemCatalog::Find(part.item_id);
    if (!stock || !item || stock->item_id != part.item_id ||
        item->usage != part.usage || !stocks.insert(part.stock_id).second ||
        part.stock_id < 1000000 || part.stock_id >= 2000000 ||
        (part.usage != InventoryUsage::kCount &&
         part.usage != InventoryUsage::kDuration) ||
        part.amount == 0 || part.max_amount == 0 ||
        (part.usage == InventoryUsage::kDuration
             ? (part.amount != 0xFFFFFFFFU && part.amount > 10000)
             : part.amount > InventorySlot::kMaxQuantity))
      throw std::invalid_argument("Invalid stored gift bundle component.");
  }
}
InventoryGrantStatus GrantGiftBundle(CharacterInventory& inventory,
                                     const GiftBundle& bundle,
                                     std::uint64_t now) {
  // Callers grant to a projection and discard the entire projection on failure.
  for (const auto& part : bundle.components) {
    if (inventory.slots.size() >=
            protocol::InventoryItemRecord::kMaxCollectionEntries &&
        std::none_of(
            inventory.slots.begin(), inventory.slots.end(),
            [&](const auto& slot) { return slot.item_id == part.item_id; }))
      return InventoryGrantStatus::kLimitExceeded;
    const auto status = InventoryGrant::Apply(
        inventory, part.item_id, part.amount, part.max_amount, now);
    if (status != InventoryGrantStatus::kGranted) return status;
  }
  return InventoryGrantStatus::kGranted;
}
}  // namespace bubble_fighter::server::features::gifts
