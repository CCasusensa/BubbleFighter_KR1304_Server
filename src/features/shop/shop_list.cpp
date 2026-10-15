#include "features/shop/shop_list.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "features/shop/shop_catalog.h"
#include "features/shop/shop_catalog_response.h"
#include "features/characters/character_creation.h"

namespace bubble_fighter::server::features::shop {
namespace {
constexpr std::uint32_t kBundleMainCategory = 800;
constexpr std::uint32_t kBundleSubCategory = 800;

std::vector<std::string> ParseCsvLine(std::string_view line) {
  std::vector<std::string> fields;
  std::string field;
  bool quoted = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char value = line[index];
    if (quoted) {
      if (value == '"') {
        if (index + 1 < line.size() && line[index + 1] == '"') {
          field.push_back('"');
          ++index;
        } else {
          quoted = false;
        }
      } else {
        field.push_back(value);
      }
    } else if (value == '"') {
      quoted = true;
    } else if (value == ',') {
      fields.push_back(std::move(field));
      field.clear();
    } else {
      field.push_back(value);
    }
  }
  fields.push_back(std::move(field));
  return fields;
}

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool ParseNumber(std::string_view text, std::uint32_t& value) {
  const auto trimmed = Trim(std::string(text));
  if (trimmed.empty()) return false;
  const auto result = std::from_chars(
      trimmed.data(), trimmed.data() + trimmed.size(), value);
  return result.ec == std::errc{} &&
         result.ptr == trimmed.data() + trimmed.size();
}

bool ParseFlag(std::string_view text) {
  const auto trimmed = Trim(std::string(text));
  return trimmed == "1" || trimmed == "true" || trimmed == "True" ||
         trimmed == "TRUE" || trimmed == "yes";
}

std::u16string ToUtf16(std::string_view text) {
  if (text.empty()) return {};
  const int units = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0);
  if (units <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(units), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), wide.data(), units);
  return std::u16string(wide.begin(), wide.end());
}

struct CsvTable {
  std::vector<std::string> header;
  std::vector<std::vector<std::string>> rows;
};

// Reads one CSV file and resolves the columns the caller needs by name.
bool ReadCsv(const std::filesystem::path& path,
             std::initializer_list<std::string_view> required,
             CsvTable& table, std::map<std::string, std::size_t>& columns,
             std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    *error = "Missing shop table file: " + path.string();
    return false;
  }
  std::string line;
  if (!std::getline(file, line)) {
    *error = "Empty shop table file: " + path.string();
    return false;
  }
  if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
      static_cast<unsigned char>(line[1]) == 0xBB &&
      static_cast<unsigned char>(line[2]) == 0xBF) {
    line.erase(0, 3);  // UTF-8 BOM written by Excel
  }
  table.header = ParseCsvLine(line);
  for (std::size_t index = 0; index < table.header.size(); ++index)
    columns.emplace(Trim(table.header[index]), index);
  for (const auto name : required) {
    if (!columns.count(std::string(name))) {
      *error = "Shop table file " + path.string() + " has no column " +
               std::string(name);
      return false;
    }
  }
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (Trim(line).empty()) continue;
    table.rows.push_back(ParseCsvLine(line));
  }
  return true;
}

const std::string& Field(const std::vector<std::string>& row,
                         const std::map<std::string, std::size_t>& columns,
                         std::string_view name) {
  static const std::string empty;
  const auto index = columns.find(std::string(name));
  if (index == columns.end() || index->second >= row.size()) return empty;
  return row[index->second];
}

std::shared_ptr<const ShopList>& active() {
  static std::shared_ptr<const ShopList> instance;
  return instance;
}

// The operator's sheet may carry a single `price` column (the generated list)
// or the resource's `show_price` / `sell_price` pair (a hand-kept export).
struct PriceColumns {
  std::string show;
  std::string sell;
};
bool ResolvePriceColumns(const std::map<std::string, std::size_t>& columns,
                         const std::filesystem::path& path,
                         PriceColumns& price, std::string* error) {
  if (columns.count("price")) {
    price.show = "price";
    price.sell = columns.count("sell_price") ? "sell_price" : "price";
    return true;
  }
  if (columns.count("show_price") || columns.count("sell_price")) {
    price.show = columns.count("show_price") ? "show_price" : "sell_price";
    price.sell = columns.count("sell_price") ? "sell_price" : "show_price";
    return true;
  }
  *error = "Shop table file " + path.string() +
           " has no price column (price / show_price / sell_price)";
  return false;
}
}  // namespace

std::optional<ShopList> ShopList::Load(const std::filesystem::path& directory,
                                       std::string* error) {
  ShopList list;
  std::map<std::string, std::size_t> item_columns;
  std::map<std::string, std::size_t> offer_columns;
  std::map<std::string, std::size_t> set_columns;
  CsvTable items;
  CsvTable offers;
  CsvTable sets;
  if (!ReadCsv(directory / "shop_items.csv",
               {"item_id", "name", "main_category", "sub_category",
                "display_order"},
               items, item_columns, error))
    return std::nullopt;
  if (!ReadCsv(directory / "shop_offers.csv",
               {"stock_id", "item_id", "currency", "duration"},
               offers, offer_columns, error))
    return std::nullopt;
  if (!ReadCsv(directory / "shop_sets.csv",
               {"stock_set_id", "name", "currency"},
               sets, set_columns, error))
    return std::nullopt;
  PriceColumns offer_price;
  PriceColumns set_price;
  if (!ResolvePriceColumns(offer_columns, directory / "shop_offers.csv",
                           offer_price, error) ||
      !ResolvePriceColumns(set_columns, directory / "shop_sets.csv",
                           set_price, error))
    return std::nullopt;

  std::map<std::uint32_t, std::size_t> by_item;
  for (const auto& row : items.rows) {
    std::uint32_t item_id = 0;
    std::uint32_t main_category = 0;
    std::uint32_t sub_category = 0;
    std::uint32_t display_order = 0;
    const auto name = ToUtf16(Trim(Field(row, item_columns, "name")));
    if (!ParseNumber(Field(row, item_columns, "item_id"), item_id) ||
        !ParseNumber(Field(row, item_columns, "main_category"), main_category) ||
        !ParseNumber(Field(row, item_columns, "sub_category"), sub_category) ||
        !ParseNumber(Field(row, item_columns, "display_order"), display_order) ||
        item_id == 0 || name.empty()) {
      ++list.skipped_rows_;
      continue;
    }
    ShopListEntry entry;
    entry.identifier = item_id;
    entry.kind = 0;
    entry.name = name;
    entry.display_order = display_order;
    entry.main_category = main_category;
    entry.sub_category = sub_category;
    by_item.emplace(item_id, list.entries_.size());
    list.entries_.push_back(std::move(entry));
  }

  for (const auto& row : offers.rows) {
    std::uint32_t stock_id = 0;
    std::uint32_t item_id = 0;
    std::uint32_t currency = 0;
    std::uint32_t duration = 0;
    std::uint32_t show_price = 0;
    std::uint32_t sell_price = 0;
    if (!ParseNumber(Field(row, offer_columns, "stock_id"), stock_id) ||
        !ParseNumber(Field(row, offer_columns, "item_id"), item_id) ||
        !ParseNumber(Field(row, offer_columns, "currency"), currency) ||
        !ParseNumber(Field(row, offer_columns, "duration"), duration) ||
        !ParseNumber(Field(row, offer_columns, offer_price.show), show_price) ||
        !ParseNumber(Field(row, offer_columns, offer_price.sell), sell_price) ||
        stock_id == 0) {
      ++list.skipped_rows_;
      continue;
    }
    const auto index = by_item.find(item_id);
    if (index == by_item.end()) {
      ++list.skipped_rows_;  // An offer for a card the table does not publish.
      continue;
    }
    protocol::ShopPriceRecord price;
    price.stock_id = stock_id;
    price.item_id = item_id;
    // E062: the client skips price choices with this flag cleared, so a
    // published offer must carry it set.
    price.flag_at12 = true;
    price.type = static_cast<std::uint16_t>(currency);
    price.duration = duration;
    price.show_price = show_price;
    price.sell_price = sell_price;
    list.entries_[index->second].prices.push_back(price);
    ++list.offer_count_;
  }

  // A card without a single usable offer cannot be rendered or bought.
  std::erase_if(list.entries_, [&](const ShopListEntry& entry) {
    if (!entry.prices.empty()) return false;
    ++list.skipped_rows_;
    return true;
  });

  for (const auto& row : sets.rows) {
    std::uint32_t set_id = 0;
    std::uint32_t currency = 0;
    std::uint32_t show_price = 0;
    std::uint32_t sell_price = 0;
    const auto name = ToUtf16(Trim(Field(row, set_columns, "name")));
    if (!ParseNumber(Field(row, set_columns, "stock_set_id"), set_id) ||
        !ParseNumber(Field(row, set_columns, "currency"), currency) ||
        !ParseNumber(Field(row, set_columns, set_price.show), show_price) ||
        !ParseNumber(Field(row, set_columns, set_price.sell), sell_price) ||
        set_id == 0 || name.empty()) {
      ++list.skipped_rows_;
      continue;
    }
    ShopListEntry entry;
    entry.identifier = set_id;
    entry.kind = 1;
    entry.name = name;
    // A bundle row names the page it belongs on, so the wardrobe's own bundle
    // sets (Clothes/SetClothes) reach the 套装 tab while ordinary packages stay
    // on Packages. Older sheets without the two columns keep 800/800.
    std::uint32_t main_category = kBundleMainCategory;
    std::uint32_t sub_category = kBundleSubCategory;
    const auto main_text = Trim(Field(row, set_columns, "main_category"));
    const auto sub_text = Trim(Field(row, set_columns, "sub_category"));
    if ((!main_text.empty() && !ParseNumber(main_text, main_category)) ||
        (!sub_text.empty() && !ParseNumber(sub_text, sub_category))) {
      ++list.skipped_rows_;
      continue;
    }
    entry.main_category = main_category;
    entry.sub_category = sub_category;
    protocol::ShopPriceRecord price;
    price.stock_id = set_id;
    price.item_id = set_id;
    // Kind 1 skips ordinary price choices; the bundle dialog reads the
    // representative price and the purchase revalidates against the table.
    price.flag_at12 = false;
    price.type = static_cast<std::uint16_t>(currency);
    price.duration = 0;
    price.show_price = show_price;
    price.sell_price = sell_price;
    entry.prices.push_back(price);
    list.entries_.push_back(std::move(entry));
  }
  if (error) error->clear();
  list.by_identifier_.clear();
  list.by_stock_.clear();
  for (std::size_t entry = 0; entry < list.entries_.size(); ++entry) {
    list.by_identifier_.emplace(list.entries_[entry].identifier, entry);
    for (std::size_t offer = 0; offer < list.entries_[entry].prices.size();
         ++offer) {
      list.by_stock_.emplace(
          list.entries_[entry].prices[offer].stock_id,
          std::make_pair(entry, offer));
    }
  }
  return list;
}

const protocol::ShopPriceRecord* ShopList::FindOffer(
    std::uint32_t stock_id) const {
  const auto found = by_stock_.find(stock_id);
  if (found == by_stock_.end()) return nullptr;
  return &entries_[found->second.first].prices[found->second.second];
}

const ShopListEntry* ShopList::FindEntry(std::uint32_t identifier) const {
  const auto found = by_identifier_.find(identifier);
  return found == by_identifier_.end() ? nullptr : &entries_[found->second];
}

void ShopList::SetActive(std::shared_ptr<const ShopList> list) {
  active() = std::move(list);
}

const ShopList* ShopList::Active() noexcept { return active().get(); }

protocol::ShopEntryResponse CreateShopListResponse(
    const ShopList& list, const protocol::ShopEntryRequest& request,
    const characters::CharacterRecord& character) {
  if (character.account_id == 0 || character.character_id == 0 ||
      !character.statistics.IsValid() ||
      !characters::CharacterCreation::SupportsSelection(
          character.character_type, character.color_index)) {
    throw std::invalid_argument(
        "Shop browsing requires a trusted persisted character.");
  }
  if (list.entries().empty()) {
    throw std::runtime_error("The shop table publishes no rows.");
  }
  protocol::ShopEntryResponse response;
  // 0x009ADA00 passes (ValueAt0 == 1) to 0x00646F80: the local catalog
  // suppresses the dynamic event-notice popup. This is not a success code.
  response.value_at0 = 1;
  response.refresh_catalog = true;
  const auto& category_definitions = ShopCatalog::Categories();
  const auto& sub_category_definitions = ShopCatalog::SubCategories();
  for (const auto& entry : list.entries()) {
    protocol::ShopItemCardRecord card;
    card.identifier = entry.identifier;
    card.kind = entry.kind;
    card.name = entry.name;
    card.value_at36 = entry.display_order;
    card.main_category = entry.main_category;
    card.sub_category = entry.sub_category;
    card.flag_at48 = true;  // current client's card visibility gate
    if (entry.kind == 0) {
      card.prices = entry.prices;
      card.price = card.prices.front();
    } else {
      card.price = entry.prices.front();
    }
    response.items.push_back(std::move(card));
  }
  for (const auto& definition : category_definitions) {
    if (!definition.visible ||
        std::none_of(response.items.begin(), response.items.end(),
                     [&](const auto& item) {
                       return item.main_category == definition.identifier;
                     })) {
      continue;
    }
    protocol::ShopMainCategoryRecord category;
    category.identifier = definition.identifier;
    category.name = definition.name;
    category.display_order = definition.display_order;
    for (const auto& child : sub_category_definitions) {
      if (child.parent_category != definition.identifier || !child.visible)
        continue;
      // Publish the page even when no card names it directly: the "All_xxx"
      // pages (page type 2) are the client's default landing page for their
      // main category and filter the pushed cards by main category themselves.
      // Dropping them left the mall blank until the player picked another tab.
      category.sub_categories.push_back(
          {child.identifier, std::u16string(child.name), child.display_order,
           child.page_type});
    }
    if (category.sub_categories.empty()) continue;  // no published page
    response.categories.push_back(std::move(category));
  }
  // Bundle cards live on the Packages tab, which the resource category list
  // does not expose as a page: append it exactly like the pinned snapshot did.
  const bool has_bundles =
      std::any_of(response.items.begin(), response.items.end(),
                  [](const auto& item) { return item.kind == 1; });
  const bool has_bundle_tab =
      std::any_of(response.categories.begin(), response.categories.end(),
                  [](const auto& category) {
                    return category.identifier == kBundleMainCategory;
                  });
  if (has_bundles && !has_bundle_tab) {
    response.categories.push_back(
        {kBundleMainCategory, u"礼包", 1006,
         {{kBundleSubCategory, u"全部礼包", 1510, 2}}});
  }
  FinalizeShopCatalogResponse(response, request);
  return response;
}
}  // namespace bubble_fighter::server::features::shop
