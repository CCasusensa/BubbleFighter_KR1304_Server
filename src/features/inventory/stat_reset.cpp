#include <algorithm>
#include <limits>
#include <stdexcept>

#include "features/characters/character_profile.h"
#include "features/inventory/item_use.h"

namespace bubble_fighter::server::features::inventory {
bool ItemUse::SupportsStatReset(const ItemDefinition& item) noexcept {
  return item.enabled && item.usage == InventoryUsage::kCount &&
         item.function == 2 &&
         ((item.item_id == 100090 && item.parameter == 1) ||
          (item.item_id == 100091 && item.parameter == 2));
}
ItemUseResult ItemUse::PrepareStatReset(
    const characters::CharacterRecord& character, CharacterInventory inventory,
    const protocol::ItemUseRequest& request,
    std::uint64_t observed_at_unix_seconds) {
  (void)request.Encode();
  auto profile = characters::CharacterProfile::BindKnownFields(
      characters::CreateCharacterProfileBaseline(), character,
      character.account_id);
  if (inventory.account_id != character.account_id ||
      inventory.character_id != character.character_id)
    throw std::invalid_argument("Item use owner/profile mismatch.");
  (void)inventory.ToRecords(character.account_id, observed_at_unix_seconds);
  const auto* item = ItemCatalog::Find(request.item_id);
  if (!item || !SupportsStatReset(*item) || request.value_at4 != 1 ||
      !request.arguments.empty() ||
      character.statistics.level < item->required_level)
    throw std::invalid_argument("Unsupported item use operation.");
  ItemUseResult result{
      character, std::move(inventory), {0, request.item_id, {}}, {}};
  auto slot = std::find_if(
      result.inventory.slots.begin(), result.inventory.slots.end(),
      [&](const auto& row) { return row.item_id == request.item_id; });
  if (slot == result.inventory.slots.end() ||
      slot->usage != InventoryUsage::kCount || slot->quantity == 0 ||
      slot->equipped ||
      slot->revision == (std::numeric_limits<std::uint64_t>::max)())
    throw std::invalid_argument("Statistics reset card is unavailable.");
  if (!result.character.battle.Reset(item->parameter)) {
    result.response.result = protocol::ItemUseResponse::kResetAlreadyZero;
    result.packets = {result.response.Encode()};
    return result;
  }
  --slot->quantity;
  result.character.battle.Bind(profile.fighter);
  // Project ordering: publish the current profile before the success dialog,
  // then replace inventory. All encoding precedes either transactional write.
  result.packets = {
      profile.Encode(), result.response.Encode(),
      result.inventory
          .CreateNotification(character.account_id, observed_at_unix_seconds)
          .Encode()};
  return result;
}
}  // namespace bubble_fighter::server::features::inventory
