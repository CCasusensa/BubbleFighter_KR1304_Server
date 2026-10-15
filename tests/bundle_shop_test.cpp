#include "features/shop/bundle_shop.h"

#include <algorithm>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "crypto/sha256.h"
#include "features/inventory/default_inventory.h"
#include "features/inventory/item_catalog.h"
#include "features/shop/shop_catalog_response.h"
#include "features/shop/stock_set_catalog.h"

namespace {
namespace bf = bubble_fighter::server;
namespace shop = bf::features::shop;
namespace inv = bf::features::inventory;
namespace p = bf::protocol;
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Bundle shop line " + std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected);
}
template <class T>
void Messages(const T& result, std::initializer_list<std::uint16_t> ids) {
  Check(result.packets.size() == ids.size());
  std::size_t i = 0;
  for (const auto id : ids) Check(result.packets[i++].message_id == id);
}
}  // namespace

int RunBundleShopTests() {
  checks = 0;
  const bf::features::characters::CharacterRecord owner{
      7, 3, u"BundleBuyer", 10, 7, {100, 0, 100000, 2000000}};
  constexpr std::uint64_t now = 1800000000;
  const auto inventory = inv::DefaultInventory::Prepare(owner, {3, 7, {}}, now);
  const inv::CashInventory empty{3, 7, {}};
  for (const auto id :
       {0U, 2000003U, 2000045U, 2001722U, 2003269U, 2004901U, 2100001U}) {
    Check(!shop::PrepareBundleOffer(id));
    const auto result =
        shop::BundleShop::Purchase({id, 0}, owner, inventory, empty, 37, now);
    Check(result.purchase.response.result ==
          p::ShopPurchaseResponse::kIneligible);
    Check(result.purchase.character.statistics == owner.statistics &&
          result.pending == empty);
  }
  const auto offer = shop::PrepareBundleOffer(2000001).value();
  Check(offer.cash_price == 4500 && offer.bonus_lucci == 60000 &&
        offer.refundable);
  Check(offer.components.size() == 1 && offer.components[0].item_id == 100081 &&
        offer.components[0].amount == 100);
  const auto buy = shop::BundleShop::Purchase({2000001, 0}, owner, inventory,
                                              empty, 37, now);
  Check(buy.purchase.response.result == 1 && buy.cash_cost == 4500 &&
        buy.purchase.response.value_at40 == 37);
  Check(buy.purchase.character.statistics.cash == 1995500 &&
        buy.purchase.character.statistics.lucci == 100000);
  Check(buy.purchase.inventory.slots == inventory.slots &&
        buy.pending.slots.empty() && buy.pending.bundles.size() == 1);
  Messages(buy.purchase, {105, 810, 816});
  const auto record = buy.pending.CreateNotification(3, now).slots.at(0);
  Check(record.slot_id == 37 && record.stock_id == 2000001 &&
        record.item_id == 2000001 && record.quantity == 0 &&
        record.duration_days == 0 && record.byte_at48 == 1);
  // Independent fixed1142E40 record bytes, plus S810 count.
  const std::vector<std::uint8_t> expected{
      1, 0,   0,   0,  37, 0,   0,   0,  0, 0, 0, 0, 0, 0, 0,
      0, 129, 132, 30, 0,  129, 132, 30, 0, 0, 0, 0, 0, 1};
  Check(buy.pending.CreateNotification(3, now).Encode().payload == expected);
  auto claimed = shop::BundleShop::Pickup({37}, buy.purchase.character,
                                          inventory, buy.pending, now + 10);
  Check(claimed.response.result == 1 && claimed.pending.bundles.empty());
  Check(claimed.character.statistics.cash == 1995500 &&
        claimed.character.statistics.lucci == 160000);
  const auto item = std::find_if(
      claimed.inventory.slots.begin(), claimed.inventory.slots.end(),
      [](const auto& row) { return row.item_id == 100081; });
  Check(item != claimed.inventory.slots.end() && item->quantity == 100);
  Messages(claimed, {105, 802, 806, 835, 810});
  Check(shop::BundleShop::Pickup({37}, claimed.character, claimed.inventory,
                                 claimed.pending, now + 11)
            .response.result == 2);
  Check(shop::BundleShop::Refund({{}, 37}, claimed.character, claimed.pending,
                                 now + 11)
            .response.result == 2);
  const auto refunded = shop::BundleShop::Refund(
      {{}, 37}, buy.purchase.character, buy.pending, now + 1);
  Check(refunded.response.result == 1 && refunded.cash_returned == 4500 &&
        refunded.pending.bundles.empty());
  Check(refunded.character.statistics == owner.statistics);
  Messages(refunded, {105, 834, 824, 810});
  Check(shop::BundleShop::Refund({{}, 37}, refunded.character, refunded.pending,
                                 now + 2)
            .response.result == 2);
  Check(shop::BundleShop::Refund({u"x", 37}, buy.purchase.character,
                                 buy.pending, now + 1)
            .response.result == 2);
  Check(shop::BundleShop::Refund({{}, 37}, buy.purchase.character, buy.pending,
                                 now - 1)
            .response.result == 2);
  Check(shop::BundleShop::Refund({{}, 37}, buy.purchase.character, buy.pending,
                                 now + inv::kCashRefundWindowSeconds)
            .response.result == p::CashRefundResponse::kExpired);
  Check(buy.pending.CreateNotification(3, now + inv::kCashRefundWindowSeconds)
            .slots[0]
            .byte_at48 == 0);
  auto full = buy.purchase.character;
  full.statistics.lucci = 0x7FFFFFFFU - 59999;
  const auto failed =
      shop::BundleShop::Pickup({37}, full, inventory, buy.pending, now + 1);
  Check(failed.response.result == 2 && failed.pending == buy.pending &&
        failed.inventory.slots == inventory.slots);
  Check(
      shop::BundleShop::Purchase({2000001, 0}, full, inventory, empty, 38, now)
          .purchase.response.result == p::ShopPurchaseResponse::kIneligible);
  auto poor = owner;
  poor.statistics.cash = 4499;
  Check(
      shop::BundleShop::Purchase({2000001, 0}, poor, inventory, empty, 38, now)
          .purchase.response.result ==
      p::ShopPurchaseResponse::kInsufficientCash);
  Check(
      shop::BundleShop::Purchase({2000001, 1}, owner, inventory, empty, 38, now)
          .purchase.response.result == p::ShopPurchaseResponse::kIneligible);
  Check(shop::BundleShop::Purchase({2000001, 0}, owner, inventory, buy.pending,
                                   37, now)
            .purchase.response.result != 1);
  Rejects([&] {
    shop::BundleShop::Purchase({2000001, 0}, owner, inventory, empty, 0, now);
  });
  auto corrupt = buy.pending;
  corrupt.bundles[0].components.clear();
  corrupt.bundles[0].bonus_lucci = 0;
  Rejects([&] { corrupt.CreateNotification(3, now); });
  corrupt = buy.pending;
  corrupt.bundles.push_back(corrupt.bundles[0]);
  Rejects([&] { corrupt.CreateNotification(3, now); });
  corrupt = buy.pending;
  corrupt.character_id = 8;
  Rejects(
      [&] { shop::BundleShop::Pickup({37}, owner, inventory, corrupt, now); });
  const auto multi = shop::BundleShop::Purchase({2000105, 0}, owner, inventory,
                                                empty, 40, now);
  Check(multi.purchase.response.result == 1 &&
        multi.pending.bundles[0].components.size() == 4 &&
        multi.cash_cost == 4400);
  const auto delivered = shop::BundleShop::Pickup(
      {40}, multi.purchase.character, inventory, multi.pending, now + 123);
  Check(delivered.response.result == 1 &&
        delivered.inventory.slots.size() == inventory.slots.size() + 4);
  for (const auto id : {100111U, 100241U, 300007U}) {
    const auto row = std::find_if(
        delivered.inventory.slots.begin(), delivered.inventory.slots.end(),
        [&](const auto& value) { return value.item_id == id; });
    Check(row != delivered.inventory.slots.end() &&
          row->expires_at_unix_seconds == now + 123 + 7 * 86400);
  }
  // One permanently owned component rejects the entire bundle without partial
  // grants.
  const auto denied =
      shop::BundleShop::Pickup({40}, multi.purchase.character,
                               delivered.inventory, multi.pending, now + 124);
  Check(denied.response.result == 8 && denied.pending == multi.pending &&
        denied.inventory.slots == delivered.inventory.slots);
  Check(shop::CashShop::Purchase(
            {multi.pending.bundles[0].components[0].stock_id, 0}, owner,
            inventory, multi.pending, 41, now, true)
            .purchase.response.result != 1);
  const auto lucci =
      shop::BundleShop::Purchase({2000107, 0}, owner, inventory, empty, 0, now);
  Check(lucci.purchase.response.result == 1 &&
        lucci.purchase.lucci_cost == 4500 && lucci.cash_cost == 0);
  Check(lucci.purchase.character.statistics.lucci == 95500 &&
        lucci.purchase.character.statistics.cash == 2000000 &&
        lucci.pending == empty);
  Check(lucci.purchase.inventory.slots.size() == inventory.slots.size() + 8);
  Messages(lucci.purchase, {105, 802, 806, 816});
  const auto no_refund = shop::BundleShop::Purchase({2000812, 0}, owner,
                                                    inventory, empty, 42, now);
  Check(no_refund.purchase.response.result == 1 &&
        !no_refund.pending.bundles[0].refundable);
  Check(shop::BundleShop::Refund({{}, 42}, no_refund.purchase.character,
                                 no_refund.pending, now + 1)
            .response.result == 2);
  // Snapshot rewards survive metadata changes; neither claim nor refund
  // reprices them.
  auto snapshot = buy.pending;
  snapshot.bundles[0].cash_price = 123;
  snapshot.bundles[0].bonus_lucci = 321;
  snapshot.bundles[0].components[0].amount = 2;
  Check(shop::BundleShop::Refund({{}, 37}, buy.purchase.character, snapshot,
                                 now + 1)
            .cash_returned == 123);
  const auto saved = shop::BundleShop::Pickup({37}, buy.purchase.character,
                                              inventory, snapshot, now + 1);
  Check(saved.response.result == 1 &&
        saved.character.statistics.lucci == 100321);
  auto cash_limit = buy.purchase.character;
  cash_limit.statistics.cash = 0x7FFFFFFFU;
  const auto bounded_refund =
      shop::BundleShop::Refund({{}, 37}, cash_limit, buy.pending, now + 1);
  Check(bounded_refund.response.result == 2 &&
        bounded_refund.pending == buy.pending);
  Check(shop::CashShop::Purchase({1000239, 0}, owner, inventory, buy.pending,
                                 38, now, true)
            .purchase.response.result != 1);
  auto full_pending = empty;
  for (std::uint32_t i = 1; i <= 1024; ++i) {
    auto row = buy.pending.bundles[0];
    row.slot_id = i;
    full_pending.bundles.push_back(row);
  }
  Check(full_pending.CreateNotification(3, now).slots.size() == 1024);
  Check(shop::BundleShop::Purchase({2000105, 0}, owner, inventory, full_pending,
                                   1025, now)
            .purchase.response.result != 1);
  Check(shop::CashShop::Purchase({1000139, 0}, owner, inventory, full_pending,
                                 1025, now, true)
            .purchase.response.result != 1);
  inv::CharacterInventory full_inventory{3, 7, {}};
  for (const auto& definition : inv::ItemCatalog::Entries()) {
    if (definition.usage != inv::InventoryUsage::kDuration ||
        definition.item_id == 100081)
      continue;
    inv::InventorySlot slot;
    slot.item_id = definition.item_id;
    slot.usage = definition.usage;
    slot.quantity = 1;
    slot.expires_at_unix_seconds = now + 86400;
    full_inventory.slots.push_back(slot);
    if (full_inventory.slots.size() == 1024) break;
  }
  Check(full_inventory.slots.size() == 1024);
  const auto no_space = shop::BundleShop::Pickup(
      {37}, buy.purchase.character, full_inventory, buy.pending, now + 1);
  Check(no_space.response.result == 2 &&
        no_space.inventory.slots == full_inventory.slots &&
        no_space.pending == buy.pending);
  std::size_t eligible = 0, cash_offers = 0, bonus_offers = 0;
  for (const auto& definition : shop::StockSetCatalog::Entries()) {
    const auto candidate = shop::PrepareBundleOffer(definition.stock_set_id);
    if (!candidate) continue;
    ++eligible;
    cash_offers += definition.currency == shop::ShopCurrency::kCash;
    bonus_offers += candidate->bonus_lucci != 0;
    auto snapshot_offer = *candidate;
    snapshot_offer.slot_id = 1;
    snapshot_offer.purchased_at = now;
    inv::ValidateCashBundle(snapshot_offer);
  }
  std::cout << "Bundle policy: " << eligible << " eligible, " << cash_offers
            << " Cash, " << eligible - cash_offers << " Lucci, " << bonus_offers
            << " bonuses.\n";
  Check(eligible == 486 && cash_offers == 384 && bonus_offers == 12);
  const auto catalog = shop::CreateShopCatalogResponse({0, 800}, owner, now);
  Check(catalog.items.size() == 7555 && catalog.categories.size() == 6 &&
        catalog.selected_category == 800);
  const auto& category = catalog.categories.back();
  Check(category.identifier == 800 && category.name == u"礼包" &&
        category.display_order == 1006 && category.sub_categories.size() == 1);
  Check(category.sub_categories.front() ==
        p::ShopSubCategoryRecord{800, u"全部礼包", 1510, 2});
  std::size_t published_bundles = 0;
  for (const auto& card : catalog.items) {
    if (card.kind == 0) continue;
    const auto* definition = shop::StockSetCatalog::Find(card.identifier);
    Check(card.kind == 1 && definition != nullptr &&
          shop::PrepareBundleOffer(card.identifier).has_value());
    Check(card.name == definition->name &&
          card.value_at36 == definition->display_order &&
          card.main_category == 800 && card.sub_category == 800 &&
          card.flag_at48 && card.wide_at56 == 0 && card.wide_at64 == 0);
    Check(card.price ==
              p::ShopPriceRecord{
                  card.identifier, card.identifier, false, 1, 0,
                  static_cast<std::uint32_t>(definition->show_price),
                  static_cast<std::uint32_t>(definition->sell_price)} &&
          card.prices.empty());
    ++published_bundles;
  }
  Check(published_bundles == 486);
  for (const auto id :
       {2000003U, 2000045U, 2001722U, 2003269U, 2004901U, 2100001U})
    Check(
        std::none_of(catalog.items.begin(), catalog.items.end(),
                     [id](const auto& card) { return card.identifier == id; }));
  const auto bytes = catalog.Encode();
  Check(bytes.payload.size() > 1024 * 1024 &&
        bytes.payload.size() < p::ShopEntryResponse::kMaxPayloadBytes &&
        p::ShopEntryResponse::Decode(bytes) == catalog);
  auto ordinary = catalog;
  std::erase_if(ordinary.items,
                [](const auto& card) { return card.kind == 1; });
  ordinary.categories.pop_back();
  ordinary.catalog_revision = 0;
  ordinary.selected_category = 0;
  const auto digest = bf::crypto::HashSha256(ordinary.Encode().payload);
  Check(digest.status == bf::crypto::Sha256Status::kSuccess);
  std::uint64_t historical_revision = 0;
  for (std::size_t index = 0; index < 8; ++index)
    historical_revision |= static_cast<std::uint64_t>(digest.digest[index])
                           << (8 * index);
  Check(historical_revision == 10020168155133007003ULL &&
        catalog.catalog_revision != historical_revision);
  const auto another =
      shop::CreateShopCatalogResponse({0, 99999}, owner, now + 999);
  Check(another.selected_category == 300 && another.items == catalog.items &&
        another.categories == catalog.categories &&
        another.catalog_revision == catalog.catalog_revision);
  auto other_character = owner;
  other_character.statistics.level = 2;
  other_character.statistics.cash = 0;
  Check(shop::CreateShopCatalogResponse({0, 100}, other_character, now)
            .catalog_revision == catalog.catalog_revision);
  std::cout << "Bundle publication: " << published_bundles << " cards, "
            << bytes.payload.size() << " bytes, revision "
            << catalog.catalog_revision << ".\n";
  std::cout << "Bundle shop: " << checks << " checks passed.\n";
  return checks;
}
