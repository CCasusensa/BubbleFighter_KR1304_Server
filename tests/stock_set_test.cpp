#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>

#include "features/inventory/default_inventory.h"
#include "features/inventory/item_catalog.h"
#include "features/shop/cash_shop.h"
#include "features/shop/shop_catalog_response.h"
#include "features/shop/shop_purchase.h"
#include "features/shop/shop_stock_catalog.h"
#include "features/shop/stock_set_catalog.h"
#include "protocol/shop/shop_entry_request.h"
#include "protocol/shop/shop_entry_response.h"
#include "protocol/shop/shop_purchase_request.h"

namespace {
namespace bf = bubble_fighter::server;
namespace shop = bf::features::shop;
namespace inv = bf::features::inventory;
namespace p = bf::protocol;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("StockSet line " + std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool refused = false;
  try {
    action();
  } catch (const std::exception&) {
    refused = true;
  }
  Check(refused);
}
template <class T>
void Fixed(const T& expected, const p::TcpPacket& packet) {
  Check(T::Decode(packet) == expected);
  const auto encoded = expected.Encode();
  Check(encoded.message_id == packet.message_id &&
        encoded.payload == packet.payload);
  for (std::size_t size = 0; size < packet.payload.size(); ++size) {
    auto bad = packet;
    bad.payload.resize(size);
    Rejects([&] { (void)T::Decode(bad); });
  }
  auto bad = packet;
  bad.payload.push_back(0);
  Rejects([&] { (void)T::Decode(bad); });
  bad = packet;
  ++bad.message_id;
  Rejects([&] { (void)T::Decode(bad); });
}
}  // namespace

int RunStockSetTests(const std::filesystem::path& directory) {
  checks = 0;
  const auto rows = shop::StockSetCatalog::Entries();
  Check(rows.size() == 4110);
  Check(rows.front().stock_set_id == 2000001 &&
        rows.back().stock_set_id == 2100001);
  std::size_t cash = 0, lucci = 0, giftable = 0, refundable = 0, bonus = 0;
  std::size_t empty = 0, oversized = 0, components = 0, extended = 0;
  bool references_valid = true, sorted = true, identities_valid = true;
  std::uint32_t previous = 0;
  for (const auto& row : rows) {
    sorted &= row.stock_set_id > previous;
    previous = row.stock_set_id;
    identities_valid &= shop::StockSetCatalog::Find(row.stock_set_id) == &row;
    cash += row.currency == shop::ShopCurrency::kCash;
    lucci += row.currency == shop::ShopCurrency::kLucci;
    giftable += row.can_present;
    refundable += row.resource_refundable;
    bonus += row.bonus_lucci > 0;
    oversized +=
        row.sell_price > 2147483647ULL || row.show_price > 2147483647ULL;
    empty += std::all_of(row.stock_ids.begin(), row.stock_ids.end(),
                         [](auto id) { return id == 0; });
    for (const auto id : row.stock_ids) {
      if (!id) continue;
      ++components;
      extended += id < 1000000 || id >= 2000000;
      const auto* stock = shop::ShopStockCatalog::Find(id);
      references_valid &= stock && inv::ItemCatalog::Find(stock->item_id);
    }
  }
  Check(sorted && identities_valid && references_valid);
  Check(cash == 3080 && lucci == 1030 && giftable == 2633 &&
        refundable == 3468);
  Check(bonus == 206 && empty == 133 && oversized == 23 &&
        components == 15374 && extended == 4);
  for (const auto id : {0U, 1999999U, 2000000U, 3000000U, 0xffffffffU})
    Check(shop::StockSetCatalog::Find(id) == nullptr);

  // These values were fixed from the source workbook before generation.
  const auto& spray = *shop::StockSetCatalog::Find(2000001);
  Check(spray.name == u"婴儿粉色喷剂" &&
        spray.currency == shop::ShopCurrency::kCash);
  Check(spray.show_price == 4500 && spray.sell_price == 4500 &&
        spray.bonus_lucci == 60000);
  Check(spray.resource_listed && spray.resource_refundable &&
        spray.can_present && spray.resource_type == 1 &&
        spray.required_level == 1);
  Check(spray.allowed_characters == "all" &&
        spray.denied_characters == "none" &&
        spray.main_category == "Accessories" && spray.sub_category == "Spray" &&
        spray.display_order == 1000);
  Check(spray.available_from == 1640736000ULL &&
        spray.available_until == 1641254400ULL);
  Check(spray.stock_ids == std::array<std::uint32_t, 10>{1000239U});
  const auto& clothes = *shop::StockSetCatalog::Find(2000003);
  Check(clothes.name == u"BF棒球服套装" &&
        clothes.currency == shop::ShopCurrency::kLucci &&
        clothes.sell_price == 100 && clothes.bonus_lucci == 0 &&
        !clothes.resource_listed && clothes.can_present);
  Check(clothes.stock_ids == std::array<std::uint32_t, 10>{1000159U, 1000162U});
  const auto& halloween = *shop::StockSetCatalog::Find(2000004);
  Check(halloween.stock_ids ==
        std::array<std::uint32_t, 10>{1000332U, 1000408U, 1000411U, 1000341U,
                                      1000344U, 1000347U});
  const auto& currency = *shop::StockSetCatalog::Find(2000045);
  Check(currency.bonus_lucci == 10000 && currency.sell_price == 1 &&
        currency.stock_ids == std::array<std::uint32_t, 10>{});
  const auto& oversized_price = *shop::StockSetCatalog::Find(2001722);
  Check(oversized_price.show_price == 9999999999ULL &&
        oversized_price.sell_price == 9999999999ULL &&
        oversized_price.resource_type == 0);
  for (const auto id : {2003269U, 2003270U, 2003271U})
    Check(shop::StockSetCatalog::Find(id)->resource_type == 2);
  const auto& extended_set = *shop::StockSetCatalog::Find(2004901);
  Check(extended_set.sell_price == 4 && !extended_set.can_present &&
        !extended_set.resource_refundable);
  Check(extended_set.stock_ids ==
        std::array<std::uint32_t, 10>{900104901U, 900104902U, 900104903U,
                                      900104904U});
  Check(shop::StockSetCatalog::Find(2100001)->stock_ids ==
        std::array<std::uint32_t, 10>{1001104U, 1001105U});

  std::ifstream input(directory / "stock_set_packets.txt");
  Check(input.good());
  std::map<std::string, p::TcpPacket> fixtures;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line.front() == '#') continue;
    std::replace(line.begin(), line.end(), '|', ' ');
    std::istringstream record(line);
    std::string label, hex;
    unsigned message = 0;
    record >> label >> message >> hex;
    Check(message <= 65535 && !hex.empty() && hex.size() % 2 == 0);
    p::TcpPacket packet{static_cast<std::uint16_t>(message), {}};
    for (std::size_t i = 0; i < hex.size(); i += 2)
      packet.payload.push_back(
          static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    Check(fixtures.emplace(label, std::move(packet)).second);
  }
  Check(fixtures.size() == 8);
  Fixed(p::ShopPurchaseRequest{2000001, 0}, fixtures.at("buy_spray"));
  Fixed(p::ShopPurchaseRequest{2000003, 0}, fixtures.at("buy_clothes"));
  Fixed(p::ShopPurchaseRequest{2000045, 0}, fixtures.at("buy_bonus"));
  Fixed(p::ShopPurchaseRequest{2100001, 0}, fixtures.at("buy_test"));
  Fixed(p::ShopPurchaseRequest{2000001, 1}, fixtures.at("buy_flag_one"));
  Fixed(p::ShopPurchaseRequest{2000001, 255}, fixtures.at("buy_flag_byte"));
  for (const auto id : {2000003U, 2000001U}) {
    p::ShopEntryResponse response;
    response.value_at0 = 1;
    response.refresh_catalog = true;
    response.catalog_revision = 9;
    response.selected_category = 100;
    p::ShopItemCardRecord card;
    card.identifier = id;
    card.kind = 1;
    card.name = id == 2000003 ? u"Bundle" : std::u16string{u'A', 0, u'B'};
    card.value_at36 = 17;
    card.main_category = 100;
    card.sub_category = 107;
    card.flag_at48 = true;
    card.wide_at56 = 0x1122334455667788ULL;
    card.wide_at64 = 0xfedcba9876543210ULL;
    card.price = {id, id, true, 1, 0, 100, 100};
    if (id == 2000003) card.prices.push_back(card.price);
    response.items.push_back(card);
    Fixed(response,
          fixtures.at(id == 2000003 ? "card_bundle" : "card_bundle_opaque"));
  }

  const bf::features::characters::CharacterRecord character{
      7, 3, u"BundleBuyer", 10, 7, {100, 0, 10000, 2000000}};
  constexpr std::uint64_t now = 1800000000;
  const auto inventory =
      inv::DefaultInventory::Prepare(character, {3, 7, {}}, now);
  // Resource lookup is intentionally not publication or transaction authority.
  const auto published = shop::CreateShopCatalogResponse({}, character, now);
  Check(published.items.size() == 7555 &&
        std::count_if(published.items.begin(), published.items.end(),
                      [](const auto& item) { return item.kind == 0; }) == 7069);
  for (const auto id : {2000001U, 2000003U, 2000045U, 2001722U, 2004901U}) {
    const auto ordinary =
        shop::ShopPurchase::Prepare({id, 0}, character, inventory, now);
    Check(ordinary.response.result == p::ShopPurchaseResponse::kIneligible &&
          ordinary.character.statistics == character.statistics &&
          ordinary.inventory.slots == inventory.slots);
    const auto cash_result = shop::CashShop::Purchase(
        {id, 0}, character, inventory, {3, 7, {}}, 1, now, true);
    Check(cash_result.purchase.response.result ==
              p::ShopPurchaseResponse::kIneligible &&
          cash_result.purchase.character.statistics == character.statistics &&
          cash_result.pending.slots.empty());
  }
  std::cout << "StockSet foundation: " << checks << " checks passed.\n";
  return checks;
}
