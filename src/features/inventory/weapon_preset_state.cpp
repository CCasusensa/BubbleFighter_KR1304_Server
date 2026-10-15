#include "features/inventory/weapon_preset_state.h"

#include <algorithm>
#include <stdexcept>

#include "features/inventory/item_catalog.h"
#include "protocol/inventory/inventory_view_update_notification.h"
#include "protocol/inventory/weapon_preset_change_response.h"

namespace bubble_fighter::server::features::inventory {
namespace {
// Current resource masks. Independent slots follow the requested
// reference policy; this does not infer native rights from old SQL.
constexpr std::array<std::uint32_t, 3> kPresetMasks{1U << 18, 1U << 21,
                                                    1U << 5};
bool InSlot(std::uint32_t id, std::size_t position) {
  const auto* item = ItemCatalog::Find(id);
  return id > 100000 && id < 300000 && item &&
         item->slot_mask == kPresetMasks[position] && item->function == 0;
}
}  // namespace

void WeaponPresetState::Validate() const {
  if (primary_items.empty() ||
      primary_items.size() >
          protocol::WeaponPresetChangeRequest::kMaxPresetIndex + 1U ||
      active_index >= primary_items.size()) {
    throw std::invalid_argument("Invalid owned weapon preset pages.");
  }
  for (std::size_t i = 0; i != primary_items.size(); ++i) {
    const std::array values{primary_items[i], grenade_items[i],
                            crystal_items[i]};
    for (std::size_t position = 0; position != values.size(); ++position) {
      if (values[position] && !InSlot(values[position], position)) {
        throw std::invalid_argument("Unsupported preset slot item.");
      }
    }
  }
}

void WeaponPresetState::CaptureActive(const CharacterInventory& inventory) {
  Validate();
  std::array<std::uint32_t, 3> selected{};
  for (const auto& slot : inventory.slots) {
    if (!slot.equipped) {
      continue;
    }
    for (std::size_t position = 0; position != selected.size(); ++position) {
      if (!InSlot(slot.item_id, position)) {
        continue;
      }
      if (selected[position]) {
        throw std::invalid_argument("Conflicting active preset equipment.");
      }
      selected[position] = slot.item_id;
    }
  }
  primary_items[active_index] = selected[0];
  grenade_items[active_index] = selected[1];
  crystal_items[active_index] = selected[2];
}

protocol::WeaponPresetUpdateNotification WeaponPresetState::CreateNotification(
    const characters::CharacterRecord& character,
    const CharacterInventory& inventory,
    std::uint64_t observed_at_unix_seconds) const {
  Validate();
  if (inventory.account_id != character.account_id ||
      inventory.character_id != character.character_id ||
      character.character_type >= 32 || !character.statistics.IsValid()) {
    throw std::invalid_argument("Invalid weapon preset owner or character.");
  }
  const auto records =
      inventory.ToRecords(character.account_id, observed_at_unix_seconds);
  protocol::WeaponPresetUpdateNotification result;
  result.byte_at0 = active_index;
  result.player_id = static_cast<std::uint32_t>(character.character_id);
  for (std::size_t page = 0; page != primary_items.size(); ++page) {
    std::array values{primary_items[page], grenade_items[page],
                      crystal_items[page]};
    for (auto& id : values) {
      if (id != 0) {
        const auto slot =
            std::find_if(inventory.slots.begin(), inventory.slots.end(),
                         [&](const auto& item) { return item.item_id == id; });
        const auto* definition = ItemCatalog::Find(id);
        if (slot == inventory.slots.end()) {
          throw std::invalid_argument("Preset weapon is not owned.");
        }
        if (slot->usage != definition->usage) {
          throw std::invalid_argument("Preset weapon usage mismatch.");
        }
        // Expired/depleted/ineligible saved weapons become empty for
        // display and selection, not renewed, replaced or re-granted.
        const auto index =
            static_cast<std::size_t>(slot - inventory.slots.begin());
        if (!definition->enabled || (records[index].flags & 2) != 0 ||
            character.statistics.level < definition->required_level ||
            (definition->allowed_characters &
             (std::uint32_t{1} << character.character_type)) == 0) {
          id = 0;
        }
      }
    }
    // Native 6827C0 passes all three IDs to generic A29090 previews.
    // Showing the three independent slots is local presentation policy,
    // not a claim that the old server's wire fields have these roles.
    result.records.push_back(values);
  }
  return result;
}

WeaponPresetChangeResult PrepareWeaponPresetChange(
    const characters::CharacterRecord& character, CharacterInventory inventory,
    WeaponPresetState presets,
    const protocol::WeaponPresetChangeRequest& request,
    std::uint64_t observed_at_unix_seconds) {
  (void)request.Encode();
  presets.CaptureActive(inventory);
  if (request.object_id != protocol::WeaponPresetChangeRequest::kMenuObjectId ||
      request.preset_index >= presets.primary_items.size()) {
    throw std::invalid_argument(
        "Weapon preset is not available in the menu/plaza.");
  }
  const auto available = presets.CreateNotification(character, inventory,
                                                    observed_at_unix_seconds);
  // Persist cleanup of unavailable selections with the equipment change.
  for (std::size_t i = 0; i != available.records.size(); ++i) {
    presets.primary_items[i] = available.records[i][0];
    presets.grenade_items[i] = available.records[i][1];
    presets.crystal_items[i] = available.records[i][2];
  }
  presets.active_index = request.preset_index;
  const auto& selected = available.records[presets.active_index];
  for (auto& slot : inventory.slots) {
    for (std::size_t position = 0; position != selected.size(); ++position) {
      if (InSlot(slot.item_id, position)) {
        slot.equipped = slot.item_id == selected[position];
      }
    }
  }
  const auto view =
      EquipmentView::Create(character, inventory, observed_at_unix_seconds);
  std::vector<protocol::TcpPacket> packets;
  packets.push_back(
      inventory
          .CreateNotification(character.account_id, observed_at_unix_seconds)
          .Encode());
  packets.push_back(protocol::InventoryViewUpdateNotification{view}.Encode());
  packets.push_back(
      presets.CreateNotification(character, inventory, observed_at_unix_seconds)
          .Encode());
  packets.push_back(protocol::WeaponPresetChangeResponse{
      request.preset_index, protocol::WeaponPresetChangeResponse::kSuccess}
                        .Encode());
  return {std::move(inventory), std::move(presets), std::move(packets)};
}
}  // namespace bubble_fighter::server::features::inventory
