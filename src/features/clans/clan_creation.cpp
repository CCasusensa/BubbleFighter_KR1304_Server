#include "features/clans/clan_creation.h"

#include <algorithm>
#include <limits>

#include "features/characters/character_creation.h"
#include "features/inventory/item_catalog.h"

namespace bubble_fighter::server::features::clans {
bool ClanCreation::SupportsItem(std::uint32_t item_id) {
  const auto* item = inventory::ItemCatalog::Find(item_id);
  return item_id == 100819 && item && item->enabled && item->function == 22 &&
         item->parameter == 30 &&
         item->usage == inventory::InventoryUsage::kCount;
}

std::uint32_t ClanCreation::ValidateRequest(
    const protocol::ClanCreateRequest& request) {
  request.Encode();
  if (request.name.size() < 4 || request.name.size() > 12) return 0xffffff9cU;
  if (request.introduction.empty() || request.introduction.size() > 96)
    return 0xffffff9bU;
  if (!SupportsItem(request.item_id) || request.item_parameter != 30 ||
      request.value_after_name != 0 ||
      characters::CharacterCreation::ValidateName(request.name))
    return 0;
  // Plain text preserves UTF-16 code units. Spaces are allowed in
  // introductions; control/format/markup characters and malformed surrogate
  // pairs are not.
  for (std::size_t i = 0; i < request.introduction.size();) {
    const auto unit = request.introduction[i];
    const std::size_t count = unit >= 0xd800 && unit <= 0xdbff ? 2 : 1;
    if (unit != u' ' &&
        characters::CharacterCreation::ValidateName(
            std::u16string_view(request.introduction).substr(i, count)))
      return 0;
    i += count;
  }
  return 1;
}

bool ClanCreation::ConsumeCard(inventory::CharacterInventory& inventory,
                               std::uint32_t level, std::uint32_t item_id,
                               std::uint64_t observed_at_unix_seconds) {
  (void)inventory.ToRecords(inventory.account_id, observed_at_unix_seconds);
  if (!SupportsItem(item_id) ||
      level < inventory::ItemCatalog::Find(item_id)->required_level)
    return false;
  const auto slot =
      std::find_if(inventory.slots.begin(), inventory.slots.end(),
                   [&](const auto& row) { return row.item_id == item_id; });
  if (slot == inventory.slots.end() ||
      slot->usage != inventory::InventoryUsage::kCount || slot->equipped ||
      slot->quantity == 0 || slot->revision == 0 ||
      slot->revision == std::numeric_limits<std::uint64_t>::max())
    return false;
  --slot->quantity;
  return true;
}
}  // namespace bubble_fighter::server::features::clans
