#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "features/inventory/default_inventory.h"
#include "features/shop/cash_shop.h"
#include "features/shop/shop_list.h"
#include "features/shop/shop_purchase.h"
#include "features/shop/shop_stock_catalog.h"

namespace {
namespace shop = bubble_fighter::server::features::shop;
namespace proto = bubble_fighter::server::protocol;
int checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}

void Write(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
}

// The operator's generated list uses a single `price` column; a hand-kept
// export carries the resource pair. Both must load.
const char* kItems =
    "item_id,name,main_category,sub_category,display_order,duration_kind,money_type,on_sale\n"
    "100001,\xE6\xB5\x8B\xE8\xAF\x95\xE9\x9E\x8B,100,106,1,day,lucci,true\n"
    "100064,\xE5\x96\xB7\xE9\x9B\xBE,200,206,2,count,lucci,true\n";
const char* kOffers =
    "stock_id,item_id,currency,duration,quantity,price\n"
    "1000001,100001,1,4294967295,0,1000\n"
    "1000002,100001,9,4294967295,0,100\n"
    "1000188,100064,1,500,500,1000\n";
const char* kSets =
    "stock_set_id,name,currency,price,bonus_lucci,members\n"
    "2000001,\xE7\xA4\xBC\xE5\x8C\x85,9,100,60000,1000239\n";
}  // namespace

int RunShopListTests(const std::filesystem::path& directory) {
  const auto root = directory / "shop_list_fixture";
  std::filesystem::create_directories(root);
  Write(root / "shop_items.csv", kItems);
  Write(root / "shop_offers.csv", kOffers);
  Write(root / "shop_sets.csv", kSets);

  std::string error;
  auto list = shop::ShopList::Load(root, &error);
  Check(list.has_value(), "Shop table fixture must load");
  Check(list->entries().size() == 3 && list->offer_count() == 3 &&
            list->skipped_rows() == 0,
        "Shop table row counts");
  const auto& shoe = list->entries().front();
  Check(shoe.identifier == 100001 && shoe.kind == 0 &&
            shoe.main_category == 100 && shoe.sub_category == 106 &&
            shoe.name == u"\u6D4B\u8BD5\u978B" && shoe.prices.size() == 2,
        "Shop table item row");
  Check(shoe.prices[0].stock_id == 1000001 && shoe.prices[0].type == 1 &&
            shoe.prices[0].duration == 0xFFFFFFFFU &&
            shoe.prices[0].show_price == 1000 && shoe.prices[0].flag_at12,
        "Shop table offer row");
  const auto& bundle = list->entries().back();
  Check(bundle.identifier == 2000001 && bundle.kind == 1 &&
            bundle.main_category == 800 && bundle.sub_category == 800 &&
            bundle.prices.size() == 1 && !bundle.prices[0].flag_at12 &&
            bundle.prices[0].duration == 0 && bundle.prices[0].sell_price == 100,
        "Shop table bundle row");

  // A quantity item keeps the table's count in the duration field.
  const auto quantity = std::find_if(
      list->entries().begin(), list->entries().end(),
      [](const auto& entry) { return entry.identifier == 100064; });
  Check(quantity != list->entries().end() &&
            quantity->prices.front().duration == 500,
        "Shop table quantity tier");

  const bubble_fighter::server::features::characters::CharacterRecord character{
      7, 3, u"Tester", 10, 7, {1, 0, 1000000, 1000000}};
  proto::ShopEntryRequest request;
  request.selected_category = 100;
  const auto response =
      shop::CreateShopListResponse(*list, request, character);
  Check(response.value_at0 == 1 && response.refresh_catalog &&
            response.catalog_revision != 0 && response.items.size() == 3 &&
            response.selected_category == 100,
        "Shop list response envelope");
  Check(response.items[0].kind == 0 && response.items[0].prices.size() == 2 &&
            response.items[0].price.stock_id == 1000001 &&
            response.items[2].kind == 1 && response.items[2].prices.empty(),
        "Shop list response cards");
  Check(!response.categories.empty() &&
            response.categories.front().identifier == 100 &&
            response.categories.back().identifier == 800,
        "Shop list response tabs");
  // 聚合页（"全部"页，page type 2）必须照样发：客户端的主分类默认落在它上面，
  // 卡只挂在具体子页上，筛掉它会让商城进去是空的。
  const auto accessories = std::find_if(
      response.categories.begin(), response.categories.end(),
      [](const auto& category) { return category.identifier == 200; });
  Check(accessories != response.categories.end() &&
            std::any_of(accessories->sub_categories.begin(),
                        accessories->sub_categories.end(),
                        [](const auto& sub) {
                          return sub.identifier == 200 && sub.value_at36 == 2;
                        }),
        "Shop list keeps the aggregate page");
  // A different selection must not change the content revision.
  request.selected_category = 800;
  const auto selected =
      shop::CreateShopListResponse(*list, request, character);
  Check(selected.catalog_revision == response.catalog_revision &&
            selected.selected_category == 800,
        "Shop list selection is not part of the revision");
  // The same table twice must produce the same revision.
  Check(shop::CreateShopListResponse(*list, proto::ShopEntryRequest{}, character)
            .catalog_revision == response.catalog_revision,
        "Shop list revision is stable");

  // Missing file, missing column and a broken row.
  std::string missing_error;
  Check(!shop::ShopList::Load(directory / "shop_list_absent", &missing_error)
             .has_value() &&
            !missing_error.empty(),
        "Shop table missing directory is refused");
  Write(root / "shop_offers.csv", "stock_id,item_id,currency,duration\n");
  std::string column_error;
  Check(!shop::ShopList::Load(root, &column_error).has_value() &&
            column_error.find("price") != std::string::npos,
        "Shop table without a price column is refused");
  Write(root / "shop_offers.csv",
        "stock_id,item_id,currency,duration,price\n"
        "0,100001,1,4294967295,1000\n"
        "1000002,100001,9,4294967295,100\n");
  std::string skipped_error;
  auto skipped = shop::ShopList::Load(root, &skipped_error);
  // The unusable offer row is dropped, and the card left without any offer is
  // dropped with it: both count as skipped rows.
  Check(skipped.has_value() && skipped->skipped_rows() == 2 &&
            skipped->offer_count() == 1 && skipped->entries().size() == 2,
        "Shop table skips unusable rows");

  Write(root / "shop_items.csv",
        "item_id,name,main_category,sub_category,display_order,on_sale\n"
        "100001,Shoe,100,106,1,0\n"
        "100064,Spray,200,206,2,1\n");
  Write(root / "shop_offers.csv", kOffers);
  Write(root / "shop_sets.csv",
        "stock_set_id,name,currency,price,on_sale\n"
        "2000001,Bundle,9,100,0\n");
  auto hidden = shop::ShopList::Load(root, &error);
  Check(hidden && hidden->entries().size() == 1 &&
            hidden->offer_count() == 1 && hidden->skipped_rows() == 0,
        "Off-sale items and bundles are not malformed rows");
  Check(!hidden->FindEntry(100001) && !hidden->FindOffer(1000001) &&
            !hidden->FindOffer(1000002) && !hidden->FindEntry(2000001) &&
            hidden->FindOffer(1000188),
        "Off-sale cards cannot be browsed or bought by stock ID");
  auto editable = shop::ShopList::Load(root, &error, true);
  Check(editable && editable->entries().size() == 3 &&
            !editable->FindEntry(100001)->on_sale &&
            editable->FindEntry(100001)->prices.size() == 2,
        "Management retains off-sale offers for relisting");
  auto published = shop::ShopList::FromEntries(editable->entries());
  Check(!published.FindEntry(100001) && !published.FindEntry(2000001) &&
            published.FindEntry(100064),
        "Rebuilt snapshot excludes off-sale entries");
  Write(root / "shop_items.csv",
        "item_id,name,main_category,sub_category,display_order,on_sale\n"
        "100001,Shoe,100,106,1,invalid\n");
  Check(!shop::ShopList::Load(root, &error) &&
            error.find("on_sale") != std::string::npos,
        "Invalid sale states do not silently publish items");

  // 购买必须按表价扣款：表里把这两个真实报价改成 1000 / 100，扣款就得多这些。
  const auto* lucci_stock = shop::ShopStockCatalog::Find(1000001);
  const auto* cash_stock = shop::ShopStockCatalog::Find(1000127);
  Check(lucci_stock != nullptr && cash_stock != nullptr,
        "Pinned fixture offers exist");
  Write(root / "shop_items.csv",
        "item_id,name,main_category,sub_category,display_order\n" +
            std::to_string(lucci_stock->item_id) + ",LucciItem,200,206,1\n" +
            std::to_string(cash_stock->item_id) + ",CashItem,200,206,2\n");
  Write(root / "shop_offers.csv",
        "stock_id,item_id,currency,duration,price\n" +
            std::to_string(lucci_stock->stock_id) + "," +
            std::to_string(lucci_stock->item_id) + ",1," +
            std::to_string(lucci_stock->duration) + ",1000\n" +
            std::to_string(cash_stock->stock_id) + "," +
            std::to_string(cash_stock->item_id) + ",9," +
            std::to_string(cash_stock->duration) + ",100\n");
  Write(root / "shop_sets.csv", "stock_set_id,name,currency,price\n");
  std::string purchase_error;
  auto purchase_list = shop::ShopList::Load(root, &purchase_error);
  Check(purchase_list.has_value(), "Shop table purchase fixture must load");
  shop::ShopList::SetActive(
      std::make_shared<const shop::ShopList>(std::move(*purchase_list)));

  auto inventory =
      bubble_fighter::server::features::inventory::DefaultInventory::Prepare(
          character, {3, 7, {}}, 1000);
  auto buy = shop::ShopPurchase::Prepare({lucci_stock->stock_id, 0}, character,
                                         inventory, 1000);
  Check(buy.lucci_cost == 1000 &&
            buy.character.statistics.lucci == 1000000 - 1000,
        "Lucci purchase charges the table price");
  auto paid = shop::CashShop::Purchase({cash_stock->stock_id, 0}, character,
                                       inventory, {3, 7, {}}, 100, 1000);
  Check(paid.cash_cost == 100 &&
            paid.purchase.character.statistics.cash == 1000000 - 100,
        "Cash purchase charges the table price");
  // 别把表留给后面的测试。
  shop::ShopList::SetActive(nullptr);
  return checks;
}
