#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SHOP_STOCK_SET_CATALOG_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SHOP_STOCK_SET_CATALOG_H_

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "features/shop/shop_catalog.h"

namespace bubble_fighter::server::features::shop {
// Fingerprinted StockSet worksheet metadata. Presence does not publish an
// offer, authorize a purchase, or grant any component or bonus currency.
struct StockSetDefinition {
  std::uint32_t stock_set_id;
  std::u16string_view name;
  ShopCurrency currency;
  // Preserve oversized source prices without narrowing, wrapping or replacing
  // them with free offers. Native signed parsing is a separate UI behavior.
  std::uint64_t show_price;
  std::uint64_t sell_price;
  std::uint32_t bonus_lucci;
  bool resource_listed;
  bool resource_refundable;
  bool can_present;
  // These are resource fields, not Stock price type/duration wire fields.
  std::uint16_t resource_type;
  std::uint16_t required_level;
  std::string_view allowed_characters;
  std::string_view denied_characters;
  // Retain tokens: duplicate category names must not be resolved by assuming
  // global uniqueness or by inventing a parent from a network identifier.
  std::string_view main_category;
  std::string_view sub_category;
  std::uint32_t display_order;
  std::uint64_t available_from;
  std::uint64_t available_until;
  // Native record +228..+264: ten Stock identifiers in source order. Zero
  // entries remain zero; an empty array can still have a Lucci bonus.
  std::array<std::uint32_t, 10> stock_ids;
};

class StockSetCatalog {
 public:
  static std::span<const StockSetDefinition> Entries() noexcept;
  static const StockSetDefinition* Find(std::uint32_t stock_set_id) noexcept;
};
}  // namespace bubble_fighter::server::features::shop

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SHOP_STOCK_SET_CATALOG_H_
