#include "persistence/gifts/gift_bundle_rows.h"

#include <array>
#include <stdexcept>

namespace bubble_fighter::server::persistence::gift_sql {
using namespace my_sql_commands;
using namespace features;
void LoadBundle(MYSQL* connection, gifts::GiftRecord& gift) {
  gift.bundle.reset();
  gifts::GiftBundle bundle;
  std::uint8_t count = 0;
  {
    Statement query(
        connection,
        "SELECT BonusLucci, ComponentCount FROM GiftBundles WHERE GiftId = ?");
    std::array parameters{Number(gift.gift_id)};
    query.Execute(parameters);
    std::array columns{Number(bundle.bonus_lucci), Number(count)};
    if (!query.Fetch(columns)) return;
    if (count > 10) throw std::runtime_error("Invalid gift bundle count.");
  }
  Statement query(
      connection,
      "SELECT ComponentIndex, StockId, ItemId, UsageKind, Amount, MaxAmount FROM GiftBundleComponents WHERE GiftId = ? ORDER BY ComponentIndex LIMIT 11");
  std::array parameters{Number(gift.gift_id)};
  query.Execute(parameters);
  inventory::BundleComponent part;
  std::uint8_t index = 0, usage = 0;
  std::array columns{Number(index),        Number(part.stock_id),
                     Number(part.item_id), Number(usage),
                     Number(part.amount),  Number(part.max_amount)};
  while (query.Fetch(columns)) {
    if (index != bundle.components.size() || index >= 10)
      throw std::runtime_error("Invalid gift bundle component order.");
    part.usage = static_cast<inventory::InventoryUsage>(usage);
    bundle.components.push_back(part);
  }
  if (bundle.components.size() != count)
    throw std::runtime_error("Incomplete gift bundle snapshot.");
  gift.bundle = std::move(bundle);
}
void InsertBundle(MYSQL* connection, const gifts::GiftRecord& gift) {
  if (!gift.bundle) return;
  const auto& bundle = *gift.bundle;
  {
    Statement insert(
        connection,
        "INSERT INTO GiftBundles (GiftId, BonusLucci, ComponentCount) VALUES (?, ?, ?)");
    const auto count = static_cast<std::uint8_t>(bundle.components.size());
    std::array parameters{Number(gift.gift_id), Number(bundle.bonus_lucci),
                          Number(count)};
    insert.Execute(parameters);
    if (insert.AffectedRows() != 1)
      throw std::runtime_error("Gift bundle insert failed.");
  }
  for (std::uint8_t index = 0; index < bundle.components.size(); ++index) {
    const auto& part = bundle.components[index];
    const auto usage = static_cast<std::uint8_t>(part.usage);
    Statement insert(
        connection,
        "INSERT INTO GiftBundleComponents (GiftId, ComponentIndex, StockId, ItemId, UsageKind, Amount, MaxAmount) VALUES (?, ?, ?, ?, ?, ?, ?)");
    std::array parameters{Number(gift.gift_id),   Number(index),
                          Number(part.stock_id),  Number(part.item_id),
                          Number(usage),          Number(part.amount),
                          Number(part.max_amount)};
    insert.Execute(parameters);
    if (insert.AffectedRows() != 1)
      throw std::runtime_error("Gift bundle component insert failed.");
  }
}
}  // namespace bubble_fighter::server::persistence::gift_sql
