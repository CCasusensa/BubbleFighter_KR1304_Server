#include "features/inventory/cash_inventory.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "features/inventory/item_catalog.h"
#include "features/inventory/item_use.h"
#include "features/shop/shop_catalog.h"
#include "features/shop/shop_stock_catalog.h"

namespace bubble_fighter::server::features::inventory {
protocol::CashInventoryUpdateNotification CashInventory::CreateNotification(
    std::uint64_t authenticated_account_id,
    std::uint64_t observed_at_unix_seconds) const {
  if (account_id == 0 || account_id != authenticated_account_id ||
      character_id == 0 || character_id > 0xFFFFFFFFULL ||
      slots.size() > protocol::CashInventoryUpdateNotification::kMaxSlots ||
      bundles.size() >
          protocol::CashInventoryUpdateNotification::kMaxSlots - slots.size()) {
    throw std::invalid_argument("Invalid cash inventory owner or size.");
  }
  ValidateCashPurchases(*this);
  protocol::CashInventoryUpdateNotification result;
  std::set<std::uint32_t> identifiers;
  for (const auto& slot : slots) {
    const auto* stock = shop::ShopStockCatalog::Find(slot.stock_id);
    const auto* item = ItemCatalog::Find(slot.item_id);
    const auto* offer = shop::ShopCatalog::FindItem(slot.item_id);
    if (slot.slot_id == 0 || slot.slot_id > kMaxSlotId ||
        !identifiers.insert(slot.slot_id).second || !stock || !item || !offer ||
        (offer->currency != shop::ShopCurrency::kCash &&
         offer->currency != shop::ShopCurrency::kCashCn) ||
        stock->item_id != slot.item_id || item->usage != slot.usage ||
        (stock->duration != slot.amount &&
         !(slot.usage != InventoryUsage::kDuration && stock->duration > 65535 &&
           slot.amount <= stock->duration)) ||
        slot.max_amount == 0 || slot.cash_price > 0x7FFFFFFFU ||
        slot.purchased_at_unix_seconds > InventorySlot::kMaxUnixSeconds ||
        (slot.usage != InventoryUsage::kCount &&
         slot.usage != InventoryUsage::kDuration &&
         slot.usage != InventoryUsage::kLife) ||
        slot.amount == 0 ||
        (slot.usage != InventoryUsage::kDuration && slot.amount > 65535) ||
        (slot.usage == InventoryUsage::kDuration &&
         slot.amount != 0xFFFFFFFFU && slot.amount > 10000)) {
      throw std::invalid_argument("Unsupported stored cash inventory slot.");
    }
    protocol::CashInventorySlotRecord record;
    record.slot_id = slot.slot_id;
    record.stock_id = slot.stock_id;
    record.item_id = slot.item_id;
    if (slot.usage != InventoryUsage::kDuration) {
      record.quantity = static_cast<std::uint16_t>(slot.amount);
    } else {
      record.duration_days = slot.amount == 0xFFFFFFFFU
                                 ? 65535
                                 : static_cast<std::uint16_t>(slot.amount);
    }
    // Only a complete trusted purchase can expose the cancellation button.
    const auto receipt = std::find_if(
        purchases.begin(), purchases.end(),
        [&](const auto& row) { return row.purchase_id == slot.purchase_id; });
    record.byte_at48 =
        receipt != purchases.end() &&
                CanRefundCashPurchase(*receipt, observed_at_unix_seconds)
            ? 1
            : 0;
    // Unresolved +8/text remain zero/empty. No external billing is invoked.
    result.slots.push_back(std::move(record));
  }
  for (const auto& bundle : bundles) {
    ValidateCashBundle(bundle);
    if (!identifiers.insert(bundle.slot_id).second ||
        std::any_of(purchases.begin(), purchases.end(),
                    [&](const auto& purchase) {
                      return purchase.purchase_id == bundle.slot_id;
                    }))
      throw std::invalid_argument("Duplicate cash bundle identity.");
    protocol::CashInventorySlotRecord record;
    record.slot_id = bundle.slot_id;
    record.stock_id = bundle.stock_set_id;
    record.item_id = bundle.stock_set_id;
    record.byte_at48 =
        CanRefundCashBundle(bundle, observed_at_unix_seconds) ? 1 : 0;
    // D3CCD2: zero quantity/days selects a complete StockSet warehouse card.
    result.slots.push_back(std::move(record));
  }
  return result;
}
}  // namespace bubble_fighter::server::features::inventory
