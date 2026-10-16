#include "features/shop/bundle_shop.h"

#include <algorithm>
#include <stdexcept>

#include "features/characters/character_profile.h"
#include "features/inventory/inventory_presentation.h"
#include "features/inventory/inventory_validation.h"
#include "features/shop/shop_list.h"
#include "features/inventory/item_catalog.h"
#include "features/shop/shop_stock_catalog.h"
#include "features/shop/stock_set_catalog.h"
#include "protocol/inventory/inventory_view_update_notification.h"
#include "protocol/shop/cash_balance_response.h"

namespace bubble_fighter::server::features::shop {
namespace {
using namespace inventory;
constexpr auto kBalanceLimit = characters::CharacterStatistics::kMaxValue;
protocol::TcpPacket Profile(const characters::CharacterRecord& character) {
  return characters::CharacterProfile::BindKnownFields(
             characters::CreateCharacterProfileBaseline(), character,
             character.account_id)
      .Encode();
}
void Validate(const characters::CharacterRecord& character,
              const CashInventory& pending, std::uint64_t now) {
  if (!IsValidInventoryCharacter(character) ||
      pending.account_id != character.account_id ||
      pending.character_id != character.character_id || now == 0 ||
      now > InventorySlot::kMaxUnixSeconds)
    throw std::invalid_argument("Invalid bundle transaction owner or clock.");
  (void)pending.CreateNotification(character.account_id, now).Encode();
}
void ValidateInventory(const characters::CharacterRecord& character,
                       const CharacterInventory& inventory, std::uint64_t now) {
  if (!BelongsToCharacter(character, inventory) ||
      !MatchesItemCatalog(inventory))
    throw std::invalid_argument("Invalid bundle transaction inventory.");
  (void)inventory.ToRecords(character.account_id, now);
}
}  // namespace

std::optional<inventory::CashBundle> PrepareBundleOffer(std::uint32_t id) {
  using namespace inventory;
  const auto* set = StockSetCatalog::Find(id);
  // The published row wins over the resource price, so buying a bundle charges
  // exactly what the mall showed for it.
  const auto table = ShopList::Active();
  const auto* table_set = table ? table->FindEntry(id) : nullptr;
  if (table && (!table_set || table_set->kind != 1 || table_set->prices.empty()))
    return {};
  const auto price =
      table_set && table_set->kind == 1 && !table_set->prices.empty()
          ? static_cast<std::uint64_t>(table_set->prices.front().sell_price)
          : set ? set->sell_price : 0ULL;
  const auto* offer_currency =
      table_set && table_set->kind == 1 && !table_set->prices.empty()
          ? &table_set->prices.front()
          : nullptr;
  const bool currency_ok =
      offer_currency ? (offer_currency->type == 1 || offer_currency->type == 9)
                     : (set != nullptr &&
                        (set->currency == ShopCurrency::kCash ||
                         set->currency == ShopCurrency::kLucci));
  const bool lucci_currency_offer =
      offer_currency ? offer_currency->type == 1
                     : (set != nullptr && set->currency == ShopCurrency::kLucci);
  // The published table decides: a bundle we push is purchasable even when the
  // worksheet's own on-sale flag is clear, which is how the sets that only
  // SetItem defines become reachable. Without a table row the resource flag
  // still gates the bundle.
  const bool published_row =
      table_set != nullptr && table_set->kind == 1 && !table_set->prices.empty();
  if (!set || (!published_row && !set->resource_listed) ||
      set->resource_type != 1 ||
      price == 0 || price > kBalanceLimit ||
      set->show_price > kBalanceLimit || set->bonus_lucci > kBalanceLimit ||
      set->denied_characters != "none" ||
      !currency_ok || (lucci_currency_offer && set->bonus_lucci != 0))
    return {};
  CashBundle offer;
  offer.stock_set_id = id;
  offer.cash_price = static_cast<std::uint32_t>(price);
  offer.bonus_lucci = set->bonus_lucci;
  offer.refundable = set->resource_refundable;
  for (const auto stock_id : set->stock_ids) {
    if (stock_id == 0) continue;
    // Extended IDs fail the native Stock classifier; do not silently grant
    // them.
    if (stock_id < 1000000 || stock_id >= 2000000) return {};
    const auto* stock = ShopStockCatalog::Find(stock_id);
    const auto* item = stock ? ItemCatalog::Find(stock->item_id) : nullptr;
    const auto* definition =
        stock ? ShopCatalog::FindItem(stock->item_id) : nullptr;
    if (!stock || !item || !definition || stock->duration == 0 ||
        definition->max_amount == 0 ||
        (item->usage != InventoryUsage::kCount &&
         item->usage != InventoryUsage::kDuration &&
         item->usage != InventoryUsage::kLife) ||
        (item->usage == InventoryUsage::kDuration
             ? (stock->duration != 0xFFFFFFFFU && stock->duration > 10000)
             : stock->duration > InventorySlot::kMaxQuantity))
      return {};
    const auto maximum =
        item->usage == InventoryUsage::kCount
            ? (std::max)(definition->max_amount, stock->duration)
            : definition->max_amount;
    offer.components.push_back(
        {stock_id, stock->item_id, item->usage, stock->duration, maximum});
  }
  if (offer.components.empty() && offer.bonus_lucci == 0) return {};
  return offer;
}

CashPurchaseResult BundleShop::Purchase(
    const protocol::ShopPurchaseRequest& request,
    const characters::CharacterRecord& character,
    const inventory::CharacterInventory& inventory,
    inventory::CashInventory pending, std::uint32_t assigned_slot_id,
    std::uint64_t now) {
  using namespace inventory;
  using Response = protocol::ShopPurchaseResponse;
  Validate(character, pending, now);
  ValidateInventory(character, inventory, now);
  CashPurchaseResult result;
  result.purchase.character = character;
  result.purchase.inventory = inventory;
  result.purchase.response.stock_id = request.stock_id;
  result.pending = std::move(pending);
  const auto refuse = [&](std::uint32_t code) {
    result.purchase.response.result = code;
    result.purchase.packets = {result.purchase.response.Encode()};
    return result;
  };
  auto offer = PrepareBundleOffer(request.stock_id);
  const auto* definition = StockSetCatalog::Find(request.stock_id);
  if (!offer || request.value_at4 != 0 ||
      character.statistics.level < definition->required_level)
    return refuse(Response::kIneligible);
  const auto table = ShopList::Active();
  const auto* published = table ? table->FindEntry(request.stock_id) : nullptr;
  const bool cash = published ? published->prices.front().type == 9
                             : definition->currency == ShopCurrency::kCash;
  if ((cash ? character.statistics.cash : character.statistics.lucci) <
      offer->cash_price)
    return refuse(cash ? Response::kInsufficientCash
                       : Response::kInsufficientLucci);
  if (cash && result.pending.slots.size() + result.pending.bundles.size() >=
                  protocol::CashInventoryUpdateNotification::kMaxSlots)
    return refuse(Response::kIneligible);
  // Reserve all pending components, including new item slots and future
  // bonuses.
  auto projected = inventory;
  std::uint64_t reserved_bonus = offer->bonus_lucci;
  for (const auto& row : result.pending.slots) {
    CashBundle single;
    single.components.push_back(
        {row.stock_id, row.item_id, row.usage, row.amount, row.max_amount});
    if (GrantBundle(projected, single, now) != InventoryGrantStatus::kGranted)
      return refuse(Response::kIneligible);
  }
  for (const auto& row : result.pending.bundles) {
    if (GrantBundle(projected, row, now) != InventoryGrantStatus::kGranted)
      return refuse(Response::kIneligible);
    reserved_bonus += row.bonus_lucci;
  }
  const auto grant = GrantBundle(projected, *offer, now);
  if (grant != InventoryGrantStatus::kGranted)
    return refuse(grant == InventoryGrantStatus::kAlreadyOwned
                      ? Response::kAlreadyOwned
                      : Response::kIneligible);
  const auto after_debit =
      character.statistics.lucci - (cash ? 0U : offer->cash_price);
  if (reserved_bonus > kBalanceLimit - after_debit)
    return refuse(Response::kIneligible);
  if (cash) {
    if (assigned_slot_id == 0 || assigned_slot_id > CashInventory::kMaxSlotId ||
        std::any_of(
            result.pending.slots.begin(), result.pending.slots.end(),
            [&](const auto& row) { return row.slot_id == assigned_slot_id; }) ||
        std::any_of(
            result.pending.bundles.begin(), result.pending.bundles.end(),
            [&](const auto& row) { return row.slot_id == assigned_slot_id; }) ||
        std::any_of(result.pending.purchases.begin(),
                    result.pending.purchases.end(), [&](const auto& row) {
                      return row.purchase_id == assigned_slot_id;
                    }))
      throw std::invalid_argument("Invalid trusted bundle slot allocation.");
    offer->slot_id = assigned_slot_id;
    offer->purchased_at = now;
    result.pending.bundles.push_back(*offer);
    result.cash_cost = offer->cash_price;
    result.purchase.character.statistics.cash -= result.cash_cost;
    result.purchase.response.value_at40 = assigned_slot_id;
    result.purchase.packets = {
        Profile(result.purchase.character),
        result.pending.CreateNotification(character.account_id, now).Encode()};
  } else {
    // Pending grants only reserve capacity; deliver the newly purchased set.
    const auto status = GrantBundle(result.purchase.inventory, *offer, now);
    if (status != InventoryGrantStatus::kGranted)
      throw std::logic_error("Bundle projection disagrees with delivery.");
    result.purchase.lucci_cost = offer->cash_price;
    result.purchase.character.statistics.lucci =
        after_debit + offer->bonus_lucci;
    const auto equipment = CreateInventoryEquipment(
        result.purchase.character, result.purchase.inventory, now);
    result.purchase.packets = {
        Profile(result.purchase.character),
        result.purchase.inventory.CreateNotification(character.account_id, now)
            .Encode(),
        protocol::InventoryViewUpdateNotification{equipment}.Encode()};
  }
  result.purchase.response.result = Response::kSuccess;
  result.purchase.packets.push_back(result.purchase.response.Encode());
  return result;
}

CashPickupResult BundleShop::Pickup(
    const protocol::CashInventoryPickupRequest& request,
    const characters::CharacterRecord& character,
    inventory::CharacterInventory inventory, inventory::CashInventory pending,
    std::uint64_t now) {
  using namespace inventory;
  using Response = protocol::CashInventoryPickupResponse;
  Validate(character, pending, now);
  ValidateInventory(character, inventory, now);
  CashPickupResult result{character,
                          std::move(inventory),
                          std::move(pending),
                          {request.slot_id, Response::kFailed, 0, {}},
                          0,
                          {}};
  const auto finish = [&] {
    result.packets.push_back(result.response.Encode());
    result.packets.push_back(
        result.pending.CreateNotification(character.account_id, now).Encode());
    return result;
  };
  const auto found = std::find_if(
      result.pending.bundles.begin(), result.pending.bundles.end(),
      [&](const auto& row) { return row.slot_id == request.slot_id; });
  if (found == result.pending.bundles.end() || now < found->purchased_at ||
      found->bonus_lucci > kBalanceLimit - character.statistics.lucci)
    return finish();
  auto granted = result.inventory;
  const auto status = GrantBundle(granted, *found, now);
  if (status != InventoryGrantStatus::kGranted) {
    if (status == InventoryGrantStatus::kAlreadyOwned)
      result.response.result = Response::kAlreadyOwned;
    return finish();
  }
  result.inventory = std::move(granted);
  result.character.statistics.lucci += found->bonus_lucci;
  result.pending.bundles.erase(found);
  result.response.result = Response::kSuccess;
  const auto equipment =
      CreateInventoryEquipment(result.character, result.inventory, now);
  result.packets = {
      Profile(result.character),
      result.inventory.CreateNotification(character.account_id, now).Encode(),
      protocol::InventoryViewUpdateNotification{equipment}.Encode()};
  // S835 must see the previous bundle record; replace S810 only afterwards.
  return finish();
}

CashRefundResult BundleShop::Refund(
    const protocol::CashRefundRequest& request,
    const characters::CharacterRecord& character,
    inventory::CashInventory pending, std::uint64_t now) {
  using Response = protocol::CashRefundResponse;
  Validate(character, pending, now);
  (void)request.Encode();
  CashRefundResult result{
      character, std::move(pending), {request.slot_id, Response::kFailed, {}}};
  const auto finish = [&] {
    result.packets.push_back(result.response.Encode());
    result.packets.push_back(
        result.pending.CreateNotification(character.account_id, now).Encode());
    return result;
  };
  const auto found = std::find_if(
      result.pending.bundles.begin(), result.pending.bundles.end(),
      [&](const auto& row) { return row.slot_id == request.slot_id; });
  if (found == result.pending.bundles.end() || !request.text_at0.empty())
    return finish();
  if (found->refundable && now >= found->purchased_at &&
      now - found->purchased_at >= inventory::kCashRefundWindowSeconds) {
    result.response.result = Response::kExpired;
    return finish();
  }
  if (!CanRefundCashBundle(*found, now) ||
      found->cash_price > kBalanceLimit - character.statistics.cash)
    return finish();
  result.purchase_id = found->slot_id;
  result.cash_returned = found->cash_price;
  result.removed_slot_ids = {found->slot_id};
  result.character.statistics.cash += result.cash_returned;
  result.pending.bundles.erase(found);
  result.response.result = Response::kSuccess;
  result.packets = {
      Profile(result.character),
      protocol::CashBalanceResponse{0, result.character.statistics.cash, 0}
          .Encode()};
  return finish();
}
}  // namespace bubble_fighter::server::features::shop
