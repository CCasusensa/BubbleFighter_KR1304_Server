#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SHOP_SHOP_LIST_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SHOP_SHOP_LIST_H_

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include "features/characters/character_store.h"
#include "protocol/shop/shop_entry_request.h"
#include "protocol/shop/shop_entry_response.h"

namespace bubble_fighter::server::features::shop {
// The mall is pushed from the operator's own table, not from a pinned
// resource snapshot:
//
//   <directory>/shop_items.csv   one row per card
//   <directory>/shop_offers.csv  one row per offer (stock id + money + price)
//   <directory>/shop_sets.csv    one row per bundle card
//
// Columns are matched by header name, so the operator can reorder or extend
// the sheet in Excel. Every offer keeps the table's own stock id, so the
// published list and the purchase validation agree by construction.
struct ShopListEntry {
  std::uint32_t identifier = 0;
  std::uint32_t kind = 0;  // 0 = ordinary card, 1 = bundle card
  std::u16string name;
  std::uint32_t display_order = 0;
  std::uint32_t main_category = 0;
  std::uint32_t sub_category = 0;
  std::vector<protocol::ShopPriceRecord> prices;
};

class ShopList {
 public:
  // Returns nullopt when the directory or a required file/column is missing or
  // malformed; *error then explains why. Rows with unusable values are counted
  // in skipped_rows() instead of failing the whole table.
  static std::optional<ShopList> Load(const std::filesystem::path& directory,
                                      std::string* error);

  const std::vector<ShopListEntry>& entries() const noexcept {
    return entries_;
  }
  std::size_t offer_count() const noexcept { return offer_count_; }
  std::size_t skipped_rows() const noexcept { return skipped_rows_; }
  // Offer lookup by the offer's own stock id: browsing pushes the row and
  // purchasing charges the very same row.
  const protocol::ShopPriceRecord* FindOffer(std::uint32_t stock_id) const;
  const ShopListEntry* FindEntry(std::uint32_t identifier) const;

  // The table this server publishes from (nullptr = the pinned resource
  // snapshot). Set once at startup, read by browsing and purchasing so the
  // displayed price and the charged price cannot drift apart.
  static void SetActive(std::shared_ptr<const ShopList> list);
  static const ShopList* Active() noexcept;

 private:
  std::vector<ShopListEntry> entries_;
  std::size_t offer_count_ = 0;
  std::size_t skipped_rows_ = 0;
  std::map<std::uint32_t, std::size_t> by_identifier_;
  std::map<std::uint32_t, std::pair<std::size_t, std::size_t>> by_stock_;
};

// Builds the S800 catalog from the table. The envelope (format, revision and
// resolved selection) is the same one the pinned snapshot produces, so the
// client cannot tell the two apart.
protocol::ShopEntryResponse CreateShopListResponse(
    const ShopList& list, const protocol::ShopEntryRequest& request,
    const characters::CharacterRecord& character);
}  // namespace bubble_fighter::server::features::shop

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SHOP_SHOP_LIST_H_
