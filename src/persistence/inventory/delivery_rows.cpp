#include "persistence/inventory/delivery_rows.h"

#include "features/gifts/gift_delivery.h"
#include "persistence/gifts/gift_rows.h"

namespace bubble_fighter::server::persistence::inventory_sql {
bool CheckPurchaseDelivery(
    MYSQL* connection, const features::characters::CharacterRecord& before,
    const features::inventory::CharacterInventory& inventory,
    const features::inventory::CashInventory& pending,
    features::shop::ShopPurchaseResult& purchase, std::uint64_t now) {
  const auto gifts =
      gift_sql::LoadPending(connection, before.character_id, now);
  auto delivery =
      features::gifts::PendingGiftDelivery(before.character_id, gifts, now);
  features::inventory::AppendCashDelivery(delivery, pending);
  if (features::inventory::CheckDeliveryCapacity(
          purchase.character, purchase.inventory, delivery, now) ==
      features::inventory::InventoryGrantStatus::kGranted)
    return true;
  const auto stock_id = purchase.response.stock_id;
  purchase = {};
  purchase.character = before;
  purchase.inventory = inventory;
  purchase.response.stock_id = stock_id;
  purchase.response.result = protocol::ShopPurchaseResponse::kIneligible;
  purchase.packets = {purchase.response.Encode()};
  return false;
}
}  // namespace bubble_fighter::server::persistence::inventory_sql
