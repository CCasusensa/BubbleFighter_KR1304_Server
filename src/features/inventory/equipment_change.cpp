#include "features/inventory/equipment_change.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <utility>
#include <utility>

#include "features/inventory/item_catalog.h"
#include "protocol/inventory/inventory_view_update_notification.h"

namespace bubble_fighter::server::features::inventory {
namespace {
// E030: the client's own character item is 300000 + CharacterType, and this
// server supports the eight (type, colour) zoom pairs CharacterCreation uses.
constexpr std::uint64_t kCharacterItemBase = 300000;
constexpr std::uint64_t kCharacterItemLimit = 301000;
constexpr std::uint64_t kMaxItemId = 99'999'999;
// E072: the client's pet/article family. Every pet item in ConfigSh's
// ShopItemPet.xml carries this function id, so it is both the render gate and
// the slot: one pet at a time, and no other function family is equippable.
constexpr std::uint32_t kPetFunction = 10001;
constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 8>
    kSupportedCharacters{{{10, 7}, {19, 4}, {25, 6}, {27, 2}, {1, 1}, {2, 6},
                          {11, 2}, {12, 6}}};

// Keeps the current colour when the new type also supports it; otherwise takes
// the type's own supported colour. Nothing outside the table can be selected.
std::optional<std::uint32_t> SelectionColor(std::uint32_t type,
                                            std::uint32_t color) {
  for (const auto& pair : kSupportedCharacters) {
    if (pair.first == type && pair.second == color) return color;
  }
  for (const auto& pair : kSupportedCharacters) {
    if (pair.first == type) return pair.second;
  }
  return std::nullopt;
}
}  // namespace

EquipmentChangeResult EquipmentChange::Prepare(
    const characters::CharacterRecord& character, CharacterInventory inventory,
    std::uint64_t item_identifier, bool equip,
    std::uint64_t observed_at_unix_seconds) {
  EquipmentChangeResult result;
  result.character = character;
  if (inventory.account_id != character.account_id ||
      inventory.character_id != character.character_id ||
      !character.statistics.IsValid()) {
    throw std::invalid_argument(
        "Equipment request does not belong to a valid owned character.");
  }
  (void)inventory.ToRecords(character.account_id, observed_at_unix_seconds);
  // Every exit path carries the caller's own inventory back unchanged: the
  // storage layer refuses a batch whose slot identity moved, and an unanswered
  // equip request must not look like an inventory rewrite.
  result.inventory = std::move(inventory);
  auto& current = result.inventory;
  // Outside the catalogued item space nothing can be equipped; answer with an
  // empty batch instead of closing the connection over a client-side click.
  if (item_identifier < 100000 || item_identifier > kMaxItemId) return result;
  const auto item_id = static_cast<std::uint32_t>(item_identifier);
  const auto target =
      std::find_if(current.slots.begin(), current.slots.end(),
                   [&](const auto& slot) { return slot.item_id == item_id; });
  const auto* definition = ItemCatalog::Find(item_id);
  if (target == current.slots.end() || !definition ||
      definition->usage != target->usage) {
    return result;
  }
  const bool is_character = item_id >= kCharacterItemBase &&
                            item_id < kCharacterItemLimit;
  const bool is_ordinary = !is_character && definition->function == 0;
  if (is_character) {
    // 换角色: equipping a character item selects that fighter, and the previous
    // one is released. A character cannot be unequipped, only replaced.
    if (!equip) return result;
    const auto color = SelectionColor(item_id - kCharacterItemBase,
                                      result.character.color_index);
    if (!color) return result;
    result.character.character_type =
        static_cast<std::uint32_t>(item_id - kCharacterItemBase);
    result.character.color_index = *color;
    for (auto& slot : current.slots) {
      if (slot.item_id != item_id && slot.item_id >= kCharacterItemBase &&
          slot.item_id < kCharacterItemLimit) {
        slot.equipped = false;
      }
    }
  } else if (is_ordinary && equip) {
    // Explicit local replacement policy. The masks come from the
    // current catalog; the original server's conflict policy is unknown.
    for (auto& slot : current.slots) {
      if (!slot.equipped || slot.item_id == item_id) {
        continue;
      }
      const auto* other = ItemCatalog::Find(slot.item_id);
      if (!other || other->usage != slot.usage) {
        throw std::invalid_argument(
            "Existing equipment has an unknown catalog definition.");
      }
      if ((other->slot_mask & definition->slot_mask) != 0 ||
          ((other->part_mask | other->secondary_part_mask) &
           (definition->part_mask | definition->secondary_part_mask)) != 0) {
        slot.equipped = false;
      }
    }
  } else if (definition->function != 0) {
    // Pets are the only article family this server equips. They occupy one
    // shared slot, so equipping a second pet releases the previous one; other
    // function families (sprays, cards, booster items) are used, not worn.
    if (definition->function != kPetFunction) return result;
    if (equip) {
      for (auto& slot : current.slots) {
        if (slot.item_id == item_id || !slot.equipped) continue;
        const auto* other = ItemCatalog::Find(slot.item_id);
        if (other && other->function == definition->function)
          slot.equipped = false;
      }
    }
  }
  target->equipped = equip;
  // Validation of the final state also allows an expired or disabled
  // ordinary item to be removed without granting it a new usable period.
  const auto view =
      EquipmentView::Create(result.character, current, observed_at_unix_seconds);
  result.packets.reserve(2);
  result.packets.push_back(
      current
          .CreateNotification(character.account_id, observed_at_unix_seconds)
          .Encode());
  result.packets.push_back(
      protocol::InventoryViewUpdateNotification{view}.Encode());
  return result;
}
}  // namespace bubble_fighter::server::features::inventory
