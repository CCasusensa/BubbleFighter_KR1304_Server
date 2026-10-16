#include <algorithm>
#include <array>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "features/inventory/equipment_change.h"
#include "features/inventory/item_catalog.h"
#include "features/inventory/weapon_preset_state.h"
#include "protocol/inventory/inventory_view_update_notification.h"

namespace bf = bubble_fighter::server;
namespace inv = bf::features::inventory;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Equipment preset check at " +
                             std::to_string(at.line()));
}
template <typename Operation>
void Rejects(Operation operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected);
}
inv::InventorySlot Item(std::uint32_t id, bool equipped) {
  inv::InventorySlot result;
  result.item_id = id;
  result.usage = inv::ItemCatalog::Find(id)->usage;
  result.quantity = 3;
  result.equipped = equipped;
  return result;
}
bool Equipped(const inv::CharacterInventory& inventory, std::uint32_t id) {
  for (const auto& slot : inventory.slots) {
    if (slot.item_id == id) return slot.equipped;
  }
  throw std::runtime_error("Missing test equipment");
}
}  // namespace

int RunEquipmentPresetTests() {
  constexpr std::uint64_t now = 1800000000;
  const bf::features::characters::CharacterRecord character{17, 77, u"Loadouts",
                                                            10, 7};
  inv::CharacterInventory inventory{77, 17, {}};
  for (const auto id : {202001U, 203001U, 205002U, 205033U, 100431U, 100432U,
                        201001U, 207001U}) {
    inventory.slots.push_back(
        Item(id, id != 203001 && id != 205033 && id != 100432));
  }
  inv::WeaponPresetState state;
  state.CaptureActive(inventory);
  Check(state.CreateNotification(character, inventory, now).records[0] ==
        std::array<std::uint32_t, 3>{202001, 205002, 100431});
  // Every page saves a full independent selection; pistol/melee stay equipped.
  for (std::uint8_t page = 1; page < 5; ++page) {
    auto empty = inv::PrepareWeaponPresetChange(character, inventory, state,
                                                {0xffffffff, page}, now);
    for (auto id : {202001U, 203001U, 205002U, 205033U, 100431U, 100432U}) {
      Check(!Equipped(empty.inventory, id));
    }
    Check(Equipped(empty.inventory, 201001) &&
          Equipped(empty.inventory, 207001));
    const std::array<std::uint32_t, 3> desired =
        page % 2 ? std::array<std::uint32_t, 3>{203001, 205033, 100432}
                 : std::array<std::uint32_t, 3>{202001, 205002, 100431};
    for (auto id : desired) {
      empty.inventory = inv::EquipmentChange::Prepare(
                            character, empty.inventory, id, true, now)
                            .inventory;
    }
    empty.presets.CaptureActive(empty.inventory);
    Check(empty.presets.CreateNotification(character, empty.inventory, now)
              .records[page] == desired);
    inventory = std::move(empty.inventory);
    state = std::move(empty.presets);
  }
  for (std::uint8_t page : std::array<std::uint8_t, 6>{0, 3, 1, 4, 2, 0}) {
    const auto before = inventory;
    auto changed = inv::PrepareWeaponPresetChange(character, inventory, state,
                                                  {0xffffffff, page}, now);
    Check(changed.packets.size() == 4);
    const std::array<std::uint16_t, 4> ids{802, 806, 837, 836};
    for (std::size_t i = 0; i < ids.size(); ++i)
      Check(changed.packets[i].message_id == ids[i]);
    const auto view =
        proto::InventoryViewUpdateNotification::Decode(changed.packets[1]);
    Check(view.view.groups[0].size() == 5);
    const auto notification =
        proto::WeaponPresetUpdateNotification::Decode(changed.packets[2]);
    Check(notification.byte_at0 == page && notification.player_id == 17);
    for (auto id : notification.records[page])
      Check(Equipped(changed.inventory, id));
    Check(changed.inventory.slots[6] == before.slots[6] &&
          changed.inventory.slots[7] == before.slots[7]);
    auto repeat = inv::PrepareWeaponPresetChange(
        character, changed.inventory, changed.presets, {0xffffffff, page}, now);
    Check(repeat.inventory.slots == changed.inventory.slots &&
          repeat.presets == changed.presets);
    inventory = std::move(changed.inventory);
    state = std::move(changed.presets);
  }
  // Count-based grenade depletion, timed crystal expiry and eligibility
  // cleanup.
  inventory.slots[2].quantity = 0;
  inventory.slots[4].expires_at_unix_seconds = now;
  const auto cleaned = inv::PrepareWeaponPresetChange(
      character, inventory, state, {0xffffffff, 2}, now);
  Check(cleaned.presets.primary_items[2] == 202001 &&
        cleaned.presets.grenade_items[2] == 0 &&
        cleaned.presets.crystal_items[2] == 0);
  Check(!Equipped(cleaned.inventory, 205002) &&
        !Equipped(cleaned.inventory, 100431));
  Check(cleaned.inventory.slots[2].quantity == 0 &&
        cleaned.inventory.slots[4].expires_at_unix_seconds == now);
  for (int fault = 0; fault < 4; ++fault) {
    auto broken = state;
    if (fault == 0) broken.grenade_items[1] = 100431;
    if (fault == 1) broken.crystal_items[1] = 205033;
    if (fault == 2) broken.primary_items.resize(6);
    if (fault == 3) broken.active_index = 5;
    Rejects([&] { broken.Validate(); });
  }
  auto missing = inventory;
  missing.slots.erase(missing.slots.begin() + 3);
  Rejects([&] { state.CreateNotification(character, missing, now); });
  auto conflict = inventory;
  conflict.slots[3].equipped = true;
  Rejects([&] { state.CaptureActive(conflict); });
  // 换角色 and ArticleSlot (pets/titles/function cards) share the C804/C805
  // pair with ordinary gear. Both used to throw, which closed the session;
  // now they are modelled, and anything unknown is answered with silence.
  {
    const bf::features::characters::CharacterRecord owner{17, 77, u"Loadouts",
                                                          10, 7};
    auto bag = inv::CharacterInventory{77, 17, {}};
    bag.slots.push_back(Item(300010U, false));  // current fighter (type 10)
    bag.slots.push_back(Item(300025U, false));  // 拉比妹妹: type 25, colour 6
    bag.slots.push_back(Item(301003U, false));  // 黑蛇: a pet (function 10001)
    bag.slots.push_back(Item(301024U, false));  // 熊本熊: a second pet
    bag.slots.push_back(Item(500001U, false));  // 武器切换扩张卡: a function card
    bag.slots.push_back(Item(205002U, true));   // ordinary gear keeps working
    const auto article =
        inv::EquipmentChange::Prepare(owner, bag, 301003U, true, now);
    Check(Equipped(article.inventory, 301003U) &&
          article.character.character_type == owner.character_type &&
          article.packets.size() == 2);
    const auto article_view =
        proto::InventoryViewUpdateNotification::Decode(article.packets[1]).view;
    Check(std::any_of(article_view.groups[0].begin(),
                      article_view.groups[0].end(),
                      [](std::uint32_t id) { return id == 301003U; }));
    const auto released =
        inv::EquipmentChange::Prepare(owner, bag, 301003U, false, now);
    Check(!Equipped(released.inventory, 301003U) &&
          released.packets.size() == 2);
    // One pet at a time: equipping the second releases the first.
    auto with_pet = bag;
    with_pet.slots[2].equipped = true;
    const auto swapped =
        inv::EquipmentChange::Prepare(owner, with_pet, 301024U, true, now);
    Check(Equipped(swapped.inventory, 301024U) &&
          !Equipped(swapped.inventory, 301003U));
    const auto swapped_view =
        proto::InventoryViewUpdateNotification::Decode(swapped.packets[1]).view;
    Check(std::none_of(swapped_view.groups[0].begin(),
                       swapped_view.groups[0].end(),
                       [](std::uint32_t id) { return id == 301003U; }));
    // Function cards are used, not worn: the request is simply not answered.
    const auto card =
        inv::EquipmentChange::Prepare(owner, with_pet, 500001U, true, now);
    Check(card.packets.empty() && !Equipped(card.inventory, 500001U));
    const auto selected =
        inv::EquipmentChange::Prepare(owner, bag, 300025U, true, now);
    Check(selected.character.character_type == 25 &&
          selected.character.color_index == 6 &&
          Equipped(selected.inventory, 300025U) &&
          !Equipped(selected.inventory, 300010U) &&
          proto::InventoryViewUpdateNotification::Decode(selected.packets[1])
                  .view.value == 300025U);
    // A fighter is replaced, never unequipped.
    const auto keep =
        inv::EquipmentChange::Prepare(owner, bag, 300010U, false, now);
    Check(keep.character.character_type == owner.character_type &&
          keep.inventory.slots == bag.slots && keep.packets.empty());
    // Unknown and unowned items are never a session-ending error.
    for (const auto id : {999001U, 1234U, 205033U}) {
      const auto ignored =
          inv::EquipmentChange::Prepare(owner, bag, id, true, now);
      Check(ignored.packets.empty() && ignored.inventory.slots == bag.slots &&
            ignored.character.character_type == owner.character_type);
    }
  }
  std::cout << checks << " independent equipment preset checks passed\n";
  return checks;
}
