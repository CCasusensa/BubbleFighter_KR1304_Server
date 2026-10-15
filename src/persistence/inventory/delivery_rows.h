#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_INVENTORY_DELIVERY_ROWS_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_INVENTORY_DELIVERY_ROWS_H_

#include <mysql.h>

#include "features/inventory/cash_inventory.h"
#include "features/shop/shop_purchase.h"

namespace bubble_fighter::server::persistence::inventory_sql {
// Requires the owner's character lock and active transaction. Called before
// persistence, after candidate preparation; refusal restores the original view.
bool CheckPurchaseDelivery(
    MYSQL* connection, const features::characters::CharacterRecord& before,
    const features::inventory::CharacterInventory& inventory,
    const features::inventory::CashInventory& pending,
    features::shop::ShopPurchaseResult& purchase, std::uint64_t now);
}  // namespace bubble_fighter::server::persistence::inventory_sql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_INVENTORY_DELIVERY_ROWS_H_
