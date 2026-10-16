#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "features/characters/character_store.h"
#include "features/shop/cash_shop.h"
#include "features/shop/shop_catalog.h"
#include "features/shop/shop_list.h"
#include "features/shop/shop_purchase.h"
#include "features/shop/shop_stock_catalog.h"
#include "features/shop/stock_set_catalog.h"
#include "persistence/inventory/bundle_rows.h"
#include "persistence/inventory/delivery_rows.h"
#include "persistence/inventory/inventory_rows.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_inventory_store.h"
#include "protocol/inventory/cash_inventory_pickup_request.h"
#include "protocol/shop/shop_purchase_request.h"

namespace bubble_fighter::server::persistence {
using namespace features::inventory;
using namespace my_sql_commands;
using namespace inventory_sql;
features::shop::ShopPurchaseResult MySqlInventoryStore::Purchase(
    const features::characters::CharacterRecord& character,
    const protocol::ShopPurchaseRequest& request,
    std::uint64_t observed_at_unix_seconds) {
  auto* connection = connection_.Handle();
  Transaction transaction(connection);
  if (!LockOwner(connection, character.account_id, character.character_id)) {
    throw std::runtime_error("Purchase owner is unavailable.");
  }
  const auto current = ReadCurrentCharacter(connection, character);
  const auto inventory =
      LoadSlots(connection, character.account_id, character.character_id);
  if (features::shop::StockSetCatalog::Find(request.stock_id)) {
    auto result = PurchaseBundle(connection, current, inventory, request,
                                 observed_at_unix_seconds);
    if (result.response.result == protocol::ShopPurchaseResponse::kSuccess)
      transaction.Commit();
    return result;
  }
  const auto* stock = features::shop::ShopStockCatalog::Find(request.stock_id);
  const auto* offer =
      stock ? features::shop::ShopCatalog::FindItem(stock->item_id) : nullptr;
  // The published table decides delivery too. Only the pinned resource snapshot
  // was consulted here while ShopPurchase::Prepare already charges the
  // operator's row, so a re-issued offer (table says Lucci, resource says Cash)
  // was billed as a Lucci purchase and then filed into the Cash box. A loaded
  // table now wins; without one the resource row stays the source.
  const auto table = features::shop::ShopList::Active();
  const auto* table_offer =
      table ? table->FindOffer(request.stock_id) : nullptr;
  const bool cash_offer =
      table_offer
          ? table_offer->type == 9
          : offer &&
                (offer->currency == features::shop::ShopCurrency::kCash ||
                 offer->currency == features::shop::ShopCurrency::kCashCn);
  if (cash_offer) {
    auto pending =
        LoadCashSlots(connection, character.account_id, character.character_id);
    const auto previous_pending_count = pending.slots.size();
    std::uint32_t assigned_id = 0;
    {
      Statement allocation(
          connection,
          "SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId = 1 FOR UPDATE");
      std::array<MYSQL_BIND, 0> parameters{};
      allocation.Execute(parameters);
      std::array results{Number(assigned_id)};
      if (!allocation.Fetch(results)) {
        throw std::runtime_error("Missing cash inventory sequence.");
      }
    }
    auto result = features::shop::CashShop::Purchase(
        request, current, inventory, std::move(pending), assigned_id,
        observed_at_unix_seconds, true);
    if (result.purchase.response.result !=
        protocol::ShopPurchaseResponse::kSuccess) {
      return result.purchase;
    }
    if (!CheckPurchaseDelivery(connection, current, inventory, result.pending,
                               result.purchase, observed_at_unix_seconds))
      return result.purchase;
    if (result.cash_cost != 0) {
      Statement debit(
          connection,
          "UPDATE Characters SET Cash = ? WHERE CharacterId = ? AND AccountId = ? AND Cash = ?");
      std::array parameters{Number(result.purchase.character.statistics.cash),
                            Number(character.character_id),
                            Number(character.account_id),
                            Number(current.statistics.cash)};
      debit.Execute(parameters);
      if (debit.AffectedRows() != 1) {
        throw std::runtime_error("Cash purchase balance changed.");
      }
    }
    for (auto index = previous_pending_count;
         index < result.pending.slots.size(); ++index) {
      const auto& slot = result.pending.slots[index];
      const auto usage = static_cast<std::uint8_t>(slot.usage);
      Statement insert(
          connection,
          "INSERT INTO CashInventorySlots (SlotId, CharacterId, StockId, ItemId, UsageKind, Amount, "
          "MaxAmount, CashPrice, PurchasedAtUnixSeconds) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
      std::array parameters{Number(slot.slot_id),
                            Number(character.character_id),
                            Number(slot.stock_id),
                            Number(slot.item_id),
                            Number(usage),
                            Number(slot.amount),
                            Number(slot.max_amount),
                            Number(slot.cash_price),
                            Number(slot.purchased_at_unix_seconds)};
      insert.Execute(parameters);
      if (insert.AffectedRows() != 1) {
        throw std::runtime_error("Cash slot insert failed.");
      }
    }
    PersistCashPurchase(connection, result.pending,
                        result.pending.purchases.back());
    const auto allocated_count = static_cast<std::uint32_t>(
        result.pending.slots.size() - previous_pending_count);
    Statement advance(
        connection,
        "UPDATE CashInventorySequence SET NextSlotId = NextSlotId + ? WHERE SequenceId = 1 AND NextSlotId = ?");
    std::array advance_parameters{Number(allocated_count), Number(assigned_id)};
    advance.Execute(advance_parameters);
    if (advance.AffectedRows() != 1) {
      throw std::runtime_error("Cash slot allocation changed.");
    }
    transaction.Commit();
    return result.purchase;
  }
  auto result = features::shop::ShopPurchase::Prepare(
      request, current, inventory, observed_at_unix_seconds);
  if (result.response.result != protocol::ShopPurchaseResponse::kSuccess) {
    return result;
  }
  const auto pending =
      LoadCashSlots(connection, current.account_id, current.character_id);
  if (!CheckPurchaseDelivery(connection, current, inventory, pending, result,
                             observed_at_unix_seconds))
    return result;
  auto& slot = *std::find_if(
      result.inventory.slots.begin(), result.inventory.slots.end(),
      [&](const auto& item) { return item.item_id == result.changed_item_id; });
  if (result.lucci_cost != 0) {
    Statement debit(
        connection,
        "UPDATE Characters SET Lucci = ? WHERE CharacterId = ? AND AccountId = ? AND Lucci = ?");
    std::array parameters{Number(result.character.statistics.lucci),
                          Number(character.character_id),
                          Number(character.account_id),
                          Number(current.statistics.lucci)};
    debit.Execute(parameters);
    if (debit.AffectedRows() != 1) {
      throw std::runtime_error("Purchase balance changed during transaction.");
    }
  }
  PersistGrant(connection, character, slot);
  transaction.Commit();
  return result;
}

features::shop::CashPickupResult MySqlInventoryStore::Pickup(
    const features::characters::CharacterRecord& character,
    const protocol::CashInventoryPickupRequest& request,
    std::uint64_t observed_at_unix_seconds) {
  auto* connection = connection_.Handle();
  Transaction transaction(connection);
  if (!LockOwner(connection, character.account_id, character.character_id)) {
    throw std::runtime_error("Cash pickup owner is unavailable.");
  }
  const auto current = ReadCurrentCharacter(connection, character);
  auto pending =
      LoadCashSlots(connection, character.account_id, character.character_id);
  if (std::any_of(
          pending.bundles.begin(), pending.bundles.end(),
          [&](const auto& row) { return row.slot_id == request.slot_id; })) {
    const auto inventory =
        LoadSlots(connection, character.account_id, character.character_id);
    auto result = features::shop::BundleShop::Pickup(
        request, current, inventory, std::move(pending),
        observed_at_unix_seconds);
    if (result.response.result ==
        protocol::CashInventoryPickupResponse::kSuccess) {
      PersistBundlePickup(connection, current, inventory, result,
                          observed_at_unix_seconds);
      transaction.Commit();
    }
    return result;
  }
  const auto part = std::find_if(
      pending.slots.begin(), pending.slots.end(),
      [&](const auto& row) { return row.slot_id == request.slot_id; });
  const auto purchase_id = part == pending.slots.end() ? 0U : part->purchase_id;
  const auto purchase = std::find_if(
      pending.purchases.begin(), pending.purchases.end(),
      [&](const auto& row) { return row.purchase_id == purchase_id; });
  const auto already_claimed =
      purchase != pending.purchases.end() && purchase->claimed;
  auto result = features::shop::CashShop::Pickup(
      request, current,
      LoadSlots(connection, character.account_id, character.character_id),
      std::move(pending), observed_at_unix_seconds);
  if (result.response.result !=
      protocol::CashInventoryPickupResponse::kSuccess) {
    return result;
  }
  auto& slot =
      *std::find_if(result.inventory.slots.begin(),
                    result.inventory.slots.end(), [&](const auto& value) {
                      return value.item_id == result.changed_item_id;
                    });
  PersistGrant(connection, character, slot);
  MarkCashPurchaseClaimed(connection, character.character_id, purchase_id,
                          already_claimed);
  Statement remove(
      connection,
      "DELETE FROM CashInventorySlots WHERE SlotId = ? AND CharacterId = ?");
  std::array parameters{Number(request.slot_id),
                        Number(character.character_id)};
  remove.Execute(parameters);
  if (remove.AffectedRows() != 1) {
    throw std::runtime_error("Cash pickup slot changed.");
  }
  transaction.Commit();
  return result;
}
}  // namespace bubble_fighter::server::persistence
