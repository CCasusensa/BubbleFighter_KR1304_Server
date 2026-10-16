#include "operations/operations_internal.h"

#include <algorithm>
#include <set>
#include "features/inventory/item_catalog.h"
#include "features/shop/shop_catalog.h"
#include "features/shop/shop_list.h"
#include "features/shop/shop_stock_catalog.h"

namespace bubble_fighter::server::operations {
namespace shop = features::shop;
const Json& CatalogCategories() {
  static const Json categories = [] {
    Json result{{"mains", Json::object()}, {"subs", Json::object()}};
    for (const auto& category : shop::ShopCatalog::Categories())
      result["mains"][std::to_string(category.identifier)] = Utf8(category.name);
    for (const auto& sub : shop::ShopCatalog::SubCategories())
      result["subs"][std::to_string(sub.parent_category) + "/" +
          std::to_string(sub.identifier)] = sub.identifier == 502 ? "聊天颜色" : Utf8(sub.name);
    // ShopItem2.xml inventory-only categories are omitted from the generated
    // mall tabs, but their IDs still occur in the operator's shop CSV.
    result["mains"].update({{"0","未分类"},{"600","网吧"},{"800","礼包"},
                            {"1100","奢华俱乐部"}});
    result["subs"].update({{"0/0","未分类"},{"600/600","全部"},{"600/601","武器"},
      {"600/602","服装"},{"600/603","饰品"},{"600/604","强化武器"},
      {"600/605","强化服装"},{"800/800","全部礼包"},{"800/801","礼包"},
      {"1100/1100","全部"},{"1100/1101","奢华武器"},{"1100/1102","奢华服装"},
      {"1100/1103","其他"},{"200/603","饰品"}});
    return result;
  }();
  return categories;
}
std::string ItemName(std::uint32_t item) {
  const auto* definition = shop::ShopCatalog::FindItem(item);
  return definition ? Utf8(definition->name) : std::to_string(item);
}
std::string CharacterName(std::uint32_t type) {
  if (!type || type >= 100000) return {};
  const auto* item = shop::ShopCatalog::FindItem(300000U + type);
  return item && item->main_category == 400 && item->sub_category == 402
      ? Utf8(item->name) : std::string{};
}
Json CharacterOptions() {
  Json result = Json::array({{{"id",10},{"look",7}},{{"id",19},{"look",4}},
    {{"id",25},{"look",6}},{{"id",27},{"look",2}},{{"id",1},{"look",1}},
    {{"id",2},{"look",6}},{{"id",11},{"look",2}},{{"id",12},{"look",6}}});
  for (auto& entry : result) {
    const auto type = entry["id"].get<std::uint32_t>();
    entry["item_id"] = 300000U + type;
    entry["name"] = CharacterName(type);
  }
  return result;
}
void DescribeCategory(Json& row) {
  const auto main = std::to_string(row["main_category"].get<std::uint32_t>());
  const auto sub = std::to_string(row["sub_category"].get<std::uint32_t>());
  row["main"] = main;
  row["sub"] = sub;
  const auto& categories = CatalogCategories();
  row["main_name"] = categories["mains"].value(main, "分类 " + main);
  row["sub_name"] = categories["subs"].value(main + "/" + sub, "子分类 " + sub);
}
Json OperationsService::Impl::Catalog(MYSQL* db) {
  Json result = Json::array();
  std::map<std::uint32_t, Json> overrides;
  for (const auto& row : Query(db, "SELECT ItemId,Document FROM OperationShopEntries"))
    overrides.emplace(row["ItemId"].get<std::uint32_t>(),
                      Json::parse(row["Document"].get<std::string>()));
  std::optional<shop::ShopList> base;
  if (!shop_directory.empty()) {
    std::string error;
    base = shop::ShopList::Load(shop_directory, &error, true);
    if (!base || base->skipped_rows()) throw std::invalid_argument("Invalid base shop table: " + error);
  }
  for (const auto& definition : features::inventory::ItemCatalog::Entries()) {
    const auto* info = shop::ShopCatalog::FindItem(definition.item_id);
    const auto* card = base ? base->FindEntry(definition.item_id) : nullptr;
    Json row{{"id", definition.item_id},
      {"name", card ? Utf8(card->name) : info ? Utf8(info->name) : std::to_string(definition.item_id)},
      {"usage", static_cast<unsigned int>(definition.usage)}, {"slot_mask", definition.slot_mask},
      {"slots", Json::array({definition.slot_mask})}, {"required_level", definition.required_level},
      {"main_category", card ? card->main_category : info ? info->main_category : 0},
      {"sub_category", card ? card->sub_category : info ? info->sub_category : 0},
      {"display_order", card ? card->display_order : info ? info->display_order : 0},
      {"on_sale", false}, {"offers", Json::array()}};
    const auto append = [&](const auto& offer) {
      if (offer.type != 1 && offer.type != 9) return;
      row["offers"].push_back({{"stock_id",offer.stock_id},{"currency",offer.type},
        {"duration",offer.duration},{"show_price",offer.show_price},{"sell_price",offer.sell_price}});
    };
    if (card) for (const auto& offer : card->prices) append(offer);
    else if (!base) {
      for (const auto& offer : shop::ShopStockCatalog::Entries())
        if (offer.item_id == definition.item_id && offer.resource_listed) append(offer);
      for (const auto& offer : shop::ShopStockCatalog::FallbackEntries())
        if (offer.item_id == definition.item_id && offer.resource_listed) append(offer);
    }
    row["on_sale"] = card ? card->on_sale : !base && !row["offers"].empty();
    if (const auto found = overrides.find(definition.item_id); found != overrides.end())
      for (const auto* key : {"name","on_sale","offers","main_category","sub_category","display_order"})
        if (found->second.contains(key)) row[key] = found->second[key];
    DescribeCategory(row);
    row["money_type"] = row["offers"].empty() ? "Lucci" :
        row["offers"][0]["currency"] == 9 ? "Cash" : "Lucci";
    result.push_back(std::move(row));
  }
  return result;
}
Json OperationsService::Impl::Rebuild(MYSQL* db) {
  std::vector<shop::ShopListEntry> entries;
  std::optional<shop::ShopList> base;
  if (!shop_directory.empty()) {
    std::string error;
    base = shop::ShopList::Load(shop_directory, &error, true);
    if (!base || base->skipped_rows())
      throw std::invalid_argument("Invalid base shop table: " + error);
  }
  std::set<std::uint32_t> overridden;
  for (const auto& row : Query(db, "SELECT ItemId FROM OperationShopEntries"))
    overridden.insert(row["ItemId"].get<std::uint32_t>());
  for (const auto& row : Catalog(db)) {
    if (!row.at("on_sale").is_boolean()) throw std::invalid_argument("Expected sale flag.");
    if (!row["on_sale"].get<bool>()) continue;
    const auto id = row["id"].get<std::uint32_t>();
    const auto* original = base ? base->FindEntry(id) : nullptr;
    // Operator tables can include existing categories and synthetic stock IDs.
    if (original && !overridden.contains(id)) {
      entries.push_back(*original);
      continue;
    }
    shop::ShopListEntry card;
    card.identifier = id;
    card.name = Utf16(row.at("name").get<std::string>());
    if (card.name.empty() || card.name.size() > 128) throw std::invalid_argument("Invalid shop name.");
    card.main_category = Bounded(row,"main_category");
    card.sub_category = Bounded(row,"sub_category");
    card.display_order = Bounded(row,"display_order");
    const auto categories = shop::ShopCatalog::SubCategories();
    if (!(original && original->main_category == card.main_category &&
          original->sub_category == card.sub_category) &&
        std::none_of(categories.begin(),categories.end(),[&](const auto& sub) {
      return sub.identifier == card.sub_category && sub.parent_category == card.main_category;
    })) throw std::invalid_argument("Unknown KR shop category.");
    const auto& offers = row.at("offers");
    if (!offers.is_array() || offers.empty() || offers.size() > 32)
      throw std::invalid_argument("Select 1 to 32 offers.");
    const auto* item = features::inventory::ItemCatalog::Find(card.identifier);
    const auto* info = shop::ShopCatalog::FindItem(card.identifier);
    if (!item || !info || !info->max_amount) throw std::invalid_argument("Item cannot be sold.");
    for (const auto& offer : offers) {
      protocol::ShopPriceRecord price;
      price.stock_id = Bounded(offer,"stock_id",0,1);
      const bool existing_stock = original && std::any_of(
          original->prices.begin(), original->prices.end(),
          [&](const auto& old) { return old.stock_id == price.stock_id; });
      if (!existing_stock && (price.stock_id < 1000000 || price.stock_id >= 2000000))
        throw std::invalid_argument("Stock ID is outside the KR ordinary offer range.");
      price.item_id = card.identifier;
      price.flag_at12 = true;
      const auto currency = Bounded(offer,"currency");
      if (currency != 1 && currency != 9) throw std::invalid_argument("Unsupported currency.");
      price.type = static_cast<std::uint16_t>(currency);
      const auto duration = offer.at("duration").get<std::uint64_t>();
      if (!duration || (duration != 0xffffffffULL && duration >
          (item->usage == features::inventory::InventoryUsage::kDuration ? 10000ULL : 0x7fffffffULL)))
        throw std::invalid_argument("Invalid offer duration or quantity.");
      price.duration = static_cast<std::uint32_t>(duration);
      price.show_price = Bounded(offer,"show_price",0,1);
      price.sell_price = Bounded(offer,"sell_price",0,1);
      card.prices.push_back(price);
    }
    entries.push_back(std::move(card));
  }
  if (base) {
    for (const auto& card : base->entries()) if (card.kind == 1) entries.push_back(card);
  }
  auto snapshot = std::make_shared<const shop::ShopList>(shop::ShopList::FromEntries(std::move(entries)));
  shop::ShopList::SetActive(snapshot);
  return {{"ok",true},{"cards",snapshot->entries().size()},{"offers",snapshot->offer_count()}};
}
}  // namespace bubble_fighter::server::operations
