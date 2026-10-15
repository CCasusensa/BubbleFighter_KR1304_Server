#include <array>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "features/authentication/credential_crypto.h"
#include "features/inventory/equipment_change.h"
#include "features/inventory/item_catalog.h"
#include "features/inventory/weapon_preset_state.h"
#include "persistence/inventory/inventory_rows.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_inventory_store.h"
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
  std::cout << checks << " independent equipment preset checks passed\n";
  return checks;
}

void RunPresetMigrationTests(const std::filesystem::path& configuration,
                             bool before) {
  using namespace bf::persistence;
  using namespace bf::persistence::my_sql_commands;
  constexpr std::uint64_t now = 1800000000;
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  if (before) {
    bool rejected = false;
    try {
      MySqlInventoryStore missing(configuration);
    } catch (const MySqlError& error) {
      rejected = error.Code() == 1054;
    }
    Check(rejected);
    Check(accounts.CreateAccount(
        "preset_migration",
        bf::features::authentication::CredentialCrypto::HashPassword(
            "Synthetic-migration-password")));
    const auto account = accounts.FindAccount("preset_migration")->account_id;
    bf::features::characters::CharacterRecord character{0, account,
                                                        u"Migration", 10, 7};
    Check(characters.Create(character) ==
          bf::features::characters::CharacterCreateResult::kCreated);
    MySqlConnection connection(configuration);
    auto* mysql = connection.Handle();
    Transaction transaction(mysql);
    Check(inventory_sql::LockOwner(mysql, account, character.character_id));
    for (auto id : {202001U, 203001U, 205033U, 100431U, 201001U}) {
      auto slot = Item(id, id != 203001);
      inventory_sql::PersistGrant(mysql, character, slot);
    }
    {
      Statement insert(
          mysql,
          "INSERT INTO WeaponPresetState (CharacterId, ActiveIndex, PageCount) VALUES (?, 0, 5)");
      std::array params{Number(character.character_id)};
      insert.Execute(params);
    }
    for (std::uint8_t page = 0; page < 5; ++page) {
      Statement insert(
          mysql,
          "INSERT INTO WeaponPresetSlots (CharacterId, PresetIndex, PrimaryItemId) VALUES (?, ?, ?)");
      const std::uint32_t item = page == 0 ? 202001 : page == 4 ? 203001 : 0;
      std::array params{Number(character.character_id), Number(page),
                        Number(item)};
      insert.Execute(params);
    }
    transaction.Commit();
  } else {
    const auto account = accounts.FindAccount("preset_migration")->account_id;
    const auto character = *characters.FindByAccount(account);
    MySqlInventoryStore store(configuration);
    const auto load = [&] {
      return MySqlInventoryStore(configuration)
          .LoadSnapshot(account, character.character_id, now)
          .value();
    };
    const auto restored = load();
    Check(restored.presets.primary_items ==
          std::vector<std::uint32_t>{202001, 0, 0, 0, 203001});
    Check(restored.presets.grenade_items[0] == 205033 &&
          restored.presets.crystal_items[0] == 100431);
    Check(restored.presets.active_index == 0 &&
          restored.inventory.slots.size() == 5);
    for (std::size_t page = 1; page < 5; ++page) {
      Check(restored.presets.grenade_items[page] == 0 &&
            restored.presets.crystal_items[page] == 0);
    }
    store.ChangeWeaponPreset(character, {0xffffffff, 4}, now);
    const auto different = load();
    Check(different.presets.grenade_items[0] == 205033 &&
          different.presets.crystal_items[0] == 100431);
    Check(Equipped(different.inventory, 203001) &&
          !Equipped(different.inventory, 205033) &&
          !Equipped(different.inventory, 100431) &&
          Equipped(different.inventory, 201001));
    store.ChangeWeaponPreset(character, {0xffffffff, 0}, now);
    const auto selected = load();
    Check(Equipped(selected.inventory, 202001) &&
          Equipped(selected.inventory, 205033) &&
          Equipped(selected.inventory, 100431) &&
          Equipped(selected.inventory, 201001));
    Check(characters.FindByAccount(account) == character);
  }
  std::cout << checks
            << (before ? " pre-migration checks passed\n"
                       : " post-migration checks passed\n");
}
