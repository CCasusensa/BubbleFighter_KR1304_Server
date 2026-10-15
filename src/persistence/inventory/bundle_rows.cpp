#include "persistence/inventory/bundle_rows.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "features/shop/stock_set_catalog.h"
#include "persistence/inventory/delivery_rows.h"
#include "persistence/inventory/inventory_rows.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::persistence::inventory_sql {
using namespace features;
using namespace my_sql_commands;
namespace {
void Balances(MYSQL* connection, const characters::CharacterRecord& before,
              const characters::CharacterRecord& after) {
  if (before.statistics.cash == after.statistics.cash &&
      before.statistics.lucci == after.statistics.lucci)
    return;
  Statement update(
      connection,
      "UPDATE Characters SET Cash = ?, Lucci = ? WHERE CharacterId = ? AND AccountId = ? AND Cash = ? AND Lucci = ?");
  std::array parameters{
      Number(after.statistics.cash),  Number(after.statistics.lucci),
      Number(before.character_id),    Number(before.account_id),
      Number(before.statistics.cash), Number(before.statistics.lucci)};
  update.Execute(parameters);
  if (update.AffectedRows() != 1)
    throw std::runtime_error("Bundle balances changed.");
}
void Grants(MYSQL* connection, const characters::CharacterRecord& character,
            const inventory::CharacterInventory& before,
            inventory::CharacterInventory& after) {
  for (auto& slot : after.slots) {
    const auto found = std::find_if(
        before.slots.begin(), before.slots.end(),
        [&](const auto& row) { return row.item_id == slot.item_id; });
    if (found == before.slots.end() || *found != slot)
      PersistGrant(connection, character, slot);
  }
}
void Finish(MYSQL* connection, std::uint64_t character_id,
            std::uint32_t slot_id, std::uint8_t state, std::uint64_t now) {
  Statement update(
      connection,
      "UPDATE CashBundles SET BundleState = ?, HandledAtUnixSeconds = ? WHERE SlotId = ? AND CharacterId = ? AND BundleState = 0");
  std::array parameters{Number(state), Number(now), Number(slot_id),
                        Number(character_id)};
  update.Execute(parameters);
  if (update.AffectedRows() != 1)
    throw std::runtime_error("Bundle receipt state changed.");
}
void Insert(MYSQL* connection, std::uint64_t character_id,
            const inventory::CashBundle& bundle) {
  {
    Statement insert(
        connection,
        "INSERT INTO CashBundles (SlotId, CharacterId, StockSetId, CashPrice, BonusLucci, PurchasedAtUnixSeconds, Refundable, ComponentCount) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    const auto count = static_cast<std::uint8_t>(bundle.components.size());
    const auto refundable = static_cast<std::uint8_t>(bundle.refundable);
    std::array parameters{
        Number(bundle.slot_id),      Number(character_id),
        Number(bundle.stock_set_id), Number(bundle.cash_price),
        Number(bundle.bonus_lucci),  Number(bundle.purchased_at),
        Number(refundable),          Number(count)};
    insert.Execute(parameters);
    if (insert.AffectedRows() != 1)
      throw std::runtime_error("Bundle receipt insert failed.");
  }
  for (std::uint8_t i = 0; i < bundle.components.size(); ++i) {
    const auto& part = bundle.components[i];
    const auto usage = static_cast<std::uint8_t>(part.usage);
    Statement insert(
        connection,
        "INSERT INTO CashBundleComponents (SlotId, ComponentIndex, StockId, ItemId, UsageKind, Amount, MaxAmount) VALUES (?, ?, ?, ?, ?, ?, ?)");
    std::array parameters{Number(bundle.slot_id), Number(i),
                          Number(part.stock_id),  Number(part.item_id),
                          Number(usage),          Number(part.amount),
                          Number(part.max_amount)};
    insert.Execute(parameters);
    if (insert.AffectedRows() != 1)
      throw std::runtime_error("Bundle component insert failed.");
  }
}
}  // namespace

void LoadCashBundles(MYSQL* connection, inventory::CashInventory& inventory) {
  std::vector<std::uint8_t> counts;
  {
    Statement query(
        connection,
        "SELECT SlotId, StockSetId, CashPrice, BonusLucci, PurchasedAtUnixSeconds, Refundable, ComponentCount FROM CashBundles WHERE CharacterId = ? AND BundleState = 0 ORDER BY SlotId LIMIT 1025");
    std::array parameters{Number(inventory.character_id)};
    query.Execute(parameters);
    inventory::CashBundle bundle;
    std::uint8_t refundable = 0, count = 0;
    std::array results{Number(bundle.slot_id),
                       Number(bundle.stock_set_id),
                       Number(bundle.cash_price),
                       Number(bundle.bonus_lucci),
                       Number(bundle.purchased_at),
                       Number(refundable),
                       Number(count)};
    while (query.Fetch(results)) {
      if (refundable > 1 || count > 10 ||
          inventory.bundles.size() + inventory.slots.size() >=
              protocol::CashInventoryUpdateNotification::kMaxSlots)
        throw std::runtime_error("Invalid stored bundle header.");
      bundle.refundable = refundable != 0;
      inventory.bundles.push_back(bundle);
      counts.push_back(count);
    }
  }
  for (std::size_t i = 0; i < inventory.bundles.size(); ++i) {
    auto& bundle = inventory.bundles[i];
    Statement query(
        connection,
        "SELECT ComponentIndex, StockId, ItemId, UsageKind, Amount, MaxAmount FROM CashBundleComponents WHERE SlotId = ? ORDER BY ComponentIndex LIMIT 11");
    std::array parameters{Number(bundle.slot_id)};
    query.Execute(parameters);
    inventory::BundleComponent part;
    std::uint8_t index = 0, usage = 0;
    std::array results{Number(index),        Number(part.stock_id),
                       Number(part.item_id), Number(usage),
                       Number(part.amount),  Number(part.max_amount)};
    while (query.Fetch(results)) {
      if (index != bundle.components.size() || index >= 10)
        throw std::runtime_error("Invalid stored bundle component order.");
      part.usage = static_cast<inventory::InventoryUsage>(usage);
      bundle.components.push_back(part);
    }
    if (bundle.components.size() != counts[i])
      throw std::runtime_error("Incomplete stored bundle receipt.");
    inventory::ValidateCashBundle(bundle);
  }
}

shop::ShopPurchaseResult PurchaseBundle(
    MYSQL* connection, const characters::CharacterRecord& character,
    const inventory::CharacterInventory& inventory,
    const protocol::ShopPurchaseRequest& request, std::uint64_t now) {
  auto pending =
      LoadCashSlots(connection, character.account_id, character.character_id);
  const auto* definition = shop::StockSetCatalog::Find(request.stock_id);
  std::uint32_t assigned = 0;
  if (definition && definition->currency == shop::ShopCurrency::kCash) {
    Statement query(
        connection,
        "SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId = 1 FOR UPDATE");
    query.Execute({});
    std::array results{Number(assigned)};
    if (!query.Fetch(results))
      throw std::runtime_error("Missing cash inventory sequence.");
  }
  auto result = shop::BundleShop::Purchase(request, character, inventory,
                                           std::move(pending), assigned, now);
  if (result.purchase.response.result !=
      protocol::ShopPurchaseResponse::kSuccess)
    return result.purchase;
  if (!CheckPurchaseDelivery(connection, character, inventory, result.pending,
                             result.purchase, now))
    return result.purchase;
  Balances(connection, character, result.purchase.character);
  if (assigned != 0) {
    Insert(connection, character.character_id, result.pending.bundles.back());
    Statement advance(
        connection,
        "UPDATE CashInventorySequence SET NextSlotId = NextSlotId + 1 WHERE SequenceId = 1 AND NextSlotId = ?");
    std::array parameters{Number(assigned)};
    advance.Execute(parameters);
    if (advance.AffectedRows() != 1)
      throw std::runtime_error("Bundle allocation changed.");
  } else {
    Grants(connection, character, inventory, result.purchase.inventory);
  }
  return result.purchase;
}
void PersistBundlePickup(MYSQL* connection,
                         const characters::CharacterRecord& before,
                         const inventory::CharacterInventory& inventory,
                         shop::CashPickupResult& result, std::uint64_t now) {
  Grants(connection, before, inventory, result.inventory);
  Balances(connection, before, result.character);
  Finish(connection, before.character_id, result.response.slot_id, 1, now);
}
void PersistBundleRefund(MYSQL* connection,
                         const characters::CharacterRecord& before,
                         const shop::CashRefundResult& result,
                         std::uint64_t now) {
  Balances(connection, before, result.character);
  Finish(connection, before.character_id, result.purchase_id, 2, now);
}
}  // namespace bubble_fighter::server::persistence::inventory_sql
