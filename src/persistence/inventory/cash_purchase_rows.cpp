#include <algorithm>
#include <array>
#include <stdexcept>

#include "persistence/inventory/inventory_rows.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::persistence::inventory_sql {
using namespace my_sql_commands;
using features::inventory::CashPurchase;

void LoadCashPurchases(MYSQL* connection, CashInventory& inventory) {
  Statement query(
      connection,
      "SELECT s.SlotId, p.PurchaseId, p.CharacterId, p.StockId, p.ItemId, p.UsageKind, p.Amount, p.MaxAmount, p.CashPrice, p.PurchasedAtUnixSeconds, p.SlotCount, p.PurchaseState FROM CashInventorySlots s JOIN CashPurchaseSlots m ON m.SlotId = s.SlotId JOIN CashPurchases p ON p.PurchaseId = m.PurchaseId WHERE s.CharacterId = ? ORDER BY s.SlotId LIMIT 1025");
  std::array parameters{Number(inventory.character_id)};
  query.Execute(parameters);
  CashPurchase purchase;
  std::uint32_t slot_id = 0;
  std::uint64_t owner = 0;
  std::uint8_t usage = 0, state = 0;
  std::array results{Number(slot_id),
                     Number(purchase.purchase_id),
                     Number(owner),
                     Number(purchase.stock_id),
                     Number(purchase.item_id),
                     Number(usage),
                     Number(purchase.amount),
                     Number(purchase.max_amount),
                     Number(purchase.cash_price),
                     Number(purchase.purchased_at_unix_seconds),
                     Number(purchase.slot_count),
                     Number(state)};
  while (query.Fetch(results)) {
    const auto slot =
        std::find_if(inventory.slots.begin(), inventory.slots.end(),
                     [&](const auto& row) { return row.slot_id == slot_id; });
    if (slot == inventory.slots.end() || owner != inventory.character_id ||
        state > 1) {
      throw std::runtime_error("Cash purchase ownership or state mismatch.");
    }
    slot->purchase_id = purchase.purchase_id;
    purchase.usage = static_cast<features::inventory::InventoryUsage>(usage);
    purchase.claimed = state == 1;
    const auto previous =
        std::find_if(inventory.purchases.begin(), inventory.purchases.end(),
                     [&](const auto& row) {
                       return row.purchase_id == purchase.purchase_id;
                     });
    if (previous == inventory.purchases.end())
      inventory.purchases.push_back(purchase);
    else if (*previous != purchase)
      throw std::runtime_error("Cash purchase snapshot changed.");
  }
}

void PersistCashPurchase(MYSQL* connection, const CashInventory& inventory,
                         const CashPurchase& purchase) {
  Statement insert(
      connection,
      "INSERT INTO CashPurchases (PurchaseId, CharacterId, StockId, ItemId, UsageKind, Amount, MaxAmount, CashPrice, PurchasedAtUnixSeconds, SlotCount) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  const auto usage = static_cast<std::uint8_t>(purchase.usage);
  std::array parameters{Number(purchase.purchase_id),
                        Number(inventory.character_id),
                        Number(purchase.stock_id),
                        Number(purchase.item_id),
                        Number(usage),
                        Number(purchase.amount),
                        Number(purchase.max_amount),
                        Number(purchase.cash_price),
                        Number(purchase.purchased_at_unix_seconds),
                        Number(purchase.slot_count)};
  insert.Execute(parameters);
  if (insert.AffectedRows() != 1)
    throw std::runtime_error("Cash purchase receipt insert failed.");
  for (const auto& slot : inventory.slots) {
    if (slot.purchase_id != purchase.purchase_id) continue;
    Statement part(
        connection,
        "INSERT INTO CashPurchaseSlots (SlotId, PurchaseId) VALUES (?, ?)");
    std::array values{Number(slot.slot_id), Number(purchase.purchase_id)};
    part.Execute(values);
    if (part.AffectedRows() != 1)
      throw std::runtime_error("Cash purchase part insert failed.");
  }
}

void MarkCashPurchaseClaimed(MYSQL* connection, std::uint64_t character_id,
                             std::uint32_t purchase_id, bool already_claimed) {
  if (purchase_id == 0 || already_claimed) return;
  Statement update(
      connection,
      "UPDATE CashPurchases SET PurchaseState = 1 WHERE PurchaseId = ? AND CharacterId = ? AND PurchaseState = 0");
  std::array parameters{Number(purchase_id), Number(character_id)};
  update.Execute(parameters);
  if (update.AffectedRows() != 1)
    throw std::runtime_error("Cash purchase claim state changed.");
}
}  // namespace bubble_fighter::server::persistence::inventory_sql
