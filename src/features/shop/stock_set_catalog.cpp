#include "features/shop/stock_set_catalog.h"

#include <algorithm>

#include "features/shop/generated/stock_set_catalog_data.h"

namespace bubble_fighter::server::features::shop {
std::span<const StockSetDefinition> StockSetCatalog::Entries() noexcept {
  return kStockSetCatalogData;
}

const StockSetDefinition* StockSetCatalog::Find(
    std::uint32_t stock_set_id) noexcept {
  const auto entries = Entries();
  const auto found =
      std::lower_bound(entries.begin(), entries.end(), stock_set_id,
                       [](const StockSetDefinition& row, std::uint32_t id) {
                         return row.stock_set_id < id;
                       });
  return found != entries.end() && found->stock_set_id == stock_set_id
             ? &*found
             : nullptr;
}
}  // namespace bubble_fighter::server::features::shop
