#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_INVENTORY_BUNDLE_ROWS_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_INVENTORY_BUNDLE_ROWS_H_

#include <mysql.h>

#include "features/shop/bundle_shop.h"

namespace bubble_fighter::server::persistence::inventory_sql {
void LoadCashBundles(MYSQL* connection,
                     features::inventory::CashInventory& inventory);
// These helpers require the caller's active transaction and locked owner.
features::shop::ShopPurchaseResult PurchaseBundle(
    MYSQL* connection, const features::characters::CharacterRecord& character,
    const features::inventory::CharacterInventory& inventory,
    const protocol::ShopPurchaseRequest& request, std::uint64_t now);
void PersistBundlePickup(
    MYSQL* connection, const features::characters::CharacterRecord& before,
    const features::inventory::CharacterInventory& inventory,
    features::shop::CashPickupResult& result, std::uint64_t now);
void PersistBundleRefund(MYSQL* connection,
                         const features::characters::CharacterRecord& before,
                         const features::shop::CashRefundResult& result,
                         std::uint64_t now);
}  // namespace bubble_fighter::server::persistence::inventory_sql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_INVENTORY_BUNDLE_ROWS_H_
