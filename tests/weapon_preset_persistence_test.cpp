#include <array>
#include <iostream>
#include <latch>
#include <thread>

#include "features/authentication/credential_crypto.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_inventory_store.h"
#include "protocol/inventory/weapon_preset_change_response.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/unresolved/client_to_server_message_0x0209.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bubble_fighter::server::tests {
std::size_t RunWeaponPresetPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace my_sql_commands;
  using namespace features::inventory;
  using namespace features::authentication;
  using namespace sessions;
  using namespace protocol;
  std::size_t checks = 0;
  const auto check = [&](bool ok) {
    ++checks;
    if (!ok) {
      throw std::runtime_error("Preset persistence check " +
                               std::to_string(checks) + " failed.");
    }
  };
  const auto rejects = [&](const auto& action) {
    bool failed = false;
    try {
      action();
    } catch (const std::exception&) {
      failed = true;
    }
    check(failed);
  };
  constexpr std::uint64_t now = 1'800'000'000;
  MySqlAccountStore accounts(configuration);
  const auto password =
      CredentialCrypto::HashPassword("Synthetic-preset-password");
  check(accounts.CreateAccount("preset_owner", password));
  check(accounts.CreateAccount("preset_foreign", password));
  const auto owner = accounts.FindAccount("preset_owner")->account_id;
  const auto foreign = accounts.FindAccount("preset_foreign")->account_id;
  auto characters = std::make_shared<MySqlCharacterStore>(configuration);
  features::characters::CharacterRecord character{0, owner, u"PresetTest", 10,
                                                  7};
  check(characters->Create(character) ==
        features::characters::CharacterCreateResult::kCreated);
  MySqlInventoryStore store(configuration);
  for (const auto id : {201001U, 202001U, 203001U}) {
    InventorySlot slot;
    slot.item_id = id;
    slot.usage = InventoryUsage::kDuration;
    slot.quantity = 1;
    check(store.Save(owner, character.character_id, slot, now));
  }
  const auto load = [&] {
    return MySqlInventoryStore(configuration)
        .LoadSnapshot(owner, character.character_id, now)
        .value();
  };
  auto initial = load();
  check(initial.presets.primary_items.size() == 5 &&
        initial.presets.active_index == 0);
  check(initial.presets.CreateNotification(character, initial.inventory, now)
            .records.size() == 5);
  auto equipped = store.ChangeEquipment(character, 202001, true, now);
  check(equipped.packets.size() == 3 &&
        WeaponPresetUpdateNotification::Decode(equipped.packets.back())
                .records[0][0] == 202001);
  store.ChangeEquipment(character, 201001, true, now);
  auto empty = store.ChangeWeaponPreset(character, {0xFFFFFFFF, 4}, now);
  check(empty.presets.active_index == 4 &&
        empty.presets.primary_items[0] == 202001);
  check(empty.inventory.slots[0].equipped &&
        !empty.inventory.slots[1].equipped &&
        !empty.inventory.slots[2].equipped);
  store.ChangeEquipment(character, 203001, true, now);
  auto saved = load();
  check(saved.presets.primary_items ==
        std::vector<std::uint32_t>({202001, 0, 0, 0, 203001}));
  check(saved.presets.active_index == 4 && saved.inventory.slots[2].equipped);
  const auto restored =
      store.ChangeWeaponPreset(character, {0xFFFFFFFF, 0}, now);
  check(restored.inventory.slots[0].equipped &&
        restored.inventory.slots[1].equipped &&
        !restored.inventory.slots[2].equipped);
  check(WeaponPresetChangeResponse::Decode(restored.packets.back()) ==
        WeaponPresetChangeResponse{0, 0});
  check(load().inventory.slots == restored.inventory.slots &&
        load().presets == restored.presets);
  const auto repeated =
      store.ChangeWeaponPreset(character, {0xFFFFFFFF, 0}, now);
  check(repeated.inventory.slots == restored.inventory.slots &&
        repeated.presets == restored.presets);
  auto stale = saved.inventory.slots[1];
  check(!store.Save(owner, character.character_id, stale, now));

  const auto ticket =
      [owner](std::string_view) -> std::optional<std::uint64_t> {
    return owner;
  };
  const auto menu = [&](const auto& profile, auto observed) {
    auto data =
        MySqlInventoryStore(configuration)
            .LoadSnapshot(profile.account_id, profile.character_id, observed)
            .value();
    auto result =
        sessions::CreateMenuSnapshot(profile, data.inventory, observed);
    result.presets =
        data.presets.CreateNotification(profile, data.inventory, observed);
    result.cash = data.cash;
    return result;
  };
  const auto select = [&](const auto& profile, const auto& request,
                          auto observed) {
    return MySqlInventoryStore(configuration)
        .ChangeWeaponPreset(profile, request, observed)
        .packets;
  };
  const auto initialize = [&](auto& session) {
    ClientToServerMessage0x0001 initial_record;
    initial_record.initial_value = 1256;
    LoginRequest login;
    login.nexon_passport = u"BFS1." + std::u16string(64, u'a');
    session.Process(initial_record.Encode());
    session.Process(login.Encode());
    return session.Process(ClientToServerMessage0x0209{}.Encode());
  };
  PlayerSession session{ticket, characters, menu, {}, {}, {}, {}, select};
  initialize(session);
  check(session.Process(WeaponPresetChangeRequest{0xFFFFFFFF, 4}.Encode())
            .back()
            .message_id == 836);
  PlayerSession reconnect{ticket, characters, menu, {}, {}, {}, {}, select};
  const auto reloaded = initialize(reconnect);
  check(WeaponPresetUpdateNotification::Decode(reloaded[3]).byte_at0 == 4);
  check(WeaponPresetUpdateNotification::Decode(reloaded[3]).records[0][0] ==
        202001);
  const auto before = load();
  auto not_owner = character;
  not_owner.account_id = foreign;
  rejects([&] { store.ChangeWeaponPreset(not_owner, {0xFFFFFFFF, 0}, now); });
  rejects([&] { store.ChangeWeaponPreset(character, {123, 0}, now); });
  rejects([&] { store.ChangeWeaponPreset(character, {0xFFFFFFFF, 5}, now); });
  auto stale_character = character;
  stale_character.color_index = 1;
  rejects(
      [&] { store.ChangeWeaponPreset(stale_character, {0xFFFFFFFF, 0}, now); });
  check(load().inventory.slots == before.inventory.slots &&
        load().presets == before.presets);

  std::array<WeaponPresetChangeResult, 2> results;
  std::array<std::exception_ptr, 2> errors{};
  std::latch ready(2);
  std::array<std::jthread, 2> writers;
  for (std::size_t i = 0; i != writers.size(); ++i) {
    writers[i] = std::jthread([&, i] {
      ready.arrive_and_wait();
      try {
        results[i] =
            MySqlInventoryStore(configuration)
                .ChangeWeaponPreset(
                    character, {0xFFFFFFFF, static_cast<std::uint8_t>(i * 4)},
                    now);
      } catch (...) {
        errors[i] = std::current_exception();
      }
    });
  }
  for (auto& writer : writers) {
    writer.join();
  }
  for (const auto& error : errors) {
    if (error) {
      std::rethrow_exception(error);
    }
  }
  check(results[0].presets.active_index == 0 &&
        results[0].inventory.slots[1].equipped &&
        !results[0].inventory.slots[2].equipped);
  check(results[1].presets.active_index == 4 &&
        !results[1].inventory.slots[1].equipped &&
        results[1].inventory.slots[2].equipped);
  const auto concurrent = load();
  check(concurrent.presets.primary_items[0] == 202001 &&
        concurrent.presets.primary_items[4] == 203001);
  check(concurrent.inventory.slots[1].equipped ==
            (concurrent.presets.active_index == 0) &&
        concurrent.inventory.slots[2].equipped ==
            (concurrent.presets.active_index == 4));
  store.ChangeWeaponPreset(character, {0xFFFFFFFF, 0}, now);
  auto expiring = load().inventory.slots[2];
  expiring.expires_at_unix_seconds = now;
  check(store.Save(owner, character.character_id, expiring, now));
  const auto expired = load();
  check(expired.presets.CreateNotification(character, expired.inventory, now)
            .records[4][0] == 0);
  const auto expired_selection =
      store.ChangeWeaponPreset(character, {0xFFFFFFFF, 4}, now);
  check(!expired_selection.inventory.slots[2].equipped &&
        expired_selection.presets.primary_items[4] == 0);
  check(load().inventory.slots[2].expires_at_unix_seconds == now);
  auto renewed = load().inventory.slots[2];
  renewed.expires_at_unix_seconds.reset();
  check(store.Save(owner, character.character_id, renewed, now));
  store.ChangeEquipment(character, 203001, true, now);
  MySqlConnection connection(configuration);
  // Complete loadouts survive independent connections, page switches,
  // and the same last-row failure below that rolls back equipment.
  for (const auto item : {100431U, 100432U, 205002U, 205033U}) {
    InventorySlot slot;
    slot.item_id = item;
    slot.quantity = 3;
    slot.usage =
        item == 205002 ? InventoryUsage::kCount : InventoryUsage::kDuration;
    check(store.Save(owner, character.character_id, slot, now));
  }
  const auto is_equipped = [](const CharacterInventory& bag, std::uint32_t id) {
    for (const auto& slot : bag.slots) {
      if (slot.item_id == id) {
        return slot.equipped;
      }
    }
    throw std::runtime_error("Missing loadout test item.");
  };
  store.ChangeEquipment(character, 205033, true, now);
  store.ChangeEquipment(character, 100431, true, now);
  auto complete = load();
  check(complete.presets.grenade_items[4] == 205033 &&
        complete.presets.crystal_items[4] == 100431);
  auto other_page = store.ChangeWeaponPreset(character, {0xFFFFFFFF, 0}, now);
  check(!is_equipped(other_page.inventory, 205033) &&
        !is_equipped(other_page.inventory, 100431));
  check(is_equipped(other_page.inventory, 201001));
  store.ChangeEquipment(character, 205002, true, now);
  store.ChangeEquipment(character, 100432, true, now);
  complete = load();
  check(complete.presets.grenade_items[0] == 205002 &&
        complete.presets.crystal_items[0] == 100432);
  for (const auto page : {4, 0, 4}) {
    const auto result =
        MySqlInventoryStore(configuration)
            .ChangeWeaponPreset(
                character, {0xFFFFFFFF, static_cast<std::uint8_t>(page)}, now);
    const auto page_snapshot = load();
    check(result.presets == page_snapshot.presets &&
          result.inventory.slots == page_snapshot.inventory.slots);
    check(is_equipped(page_snapshot.inventory, 205033) == (page == 4) &&
          is_equipped(page_snapshot.inventory, 205002) == (page == 0));
    check(is_equipped(page_snapshot.inventory, 100431) == (page == 4) &&
          is_equipped(page_snapshot.inventory, 100432) == (page == 0));
    check(is_equipped(page_snapshot.inventory, 201001));
  }
  store.ChangeEquipment(character, 100431, false, now);
  check(load().presets.crystal_items[4] == 0 &&
        load().presets.crystal_items[0] == 100432);
  store.ChangeEquipment(character, 100431, true, now);
  {
    Statement poison(
        connection.Handle(),
        "UPDATE WeaponPresetSlots SET PrimaryItemId = 204001 WHERE CharacterId = ? AND PresetIndex = 2");
    std::array parameters{Number(character.character_id)};
    poison.Execute(parameters);
  }
  const auto foreign_reference = load();
  rejects([&] { store.ChangeWeaponPreset(character, {0xFFFFFFFF, 0}, now); });
  check(load().inventory.slots == foreign_reference.inventory.slots &&
        load().presets == foreign_reference.presets);
  {
    Statement repair(
        connection.Handle(),
        "UPDATE WeaponPresetSlots SET PrimaryItemId = 0 WHERE CharacterId = ? AND PresetIndex = 2");
    std::array parameters{Number(character.character_id)};
    repair.Execute(parameters);
  }
  // Activate a test-only trigger which fails the last preset INSERT,
  // after inventory writes, profile update, DELETE and four row inserts.
  {
    Statement rename(
        connection.Handle(),
        "UPDATE Accounts SET Username = 'preset_rollback' WHERE AccountId = ?");
    std::array parameters{Number(owner)};
    rename.Execute(parameters);
  }
  const auto rollback = load();
  rejects([&] { store.ChangeWeaponPreset(character, {0xFFFFFFFF, 0}, now); });
  check(load().inventory.slots == rollback.inventory.slots &&
        load().presets == rollback.presets);
  rejects([&] { store.ChangeEquipment(character, 202001, true, now); });
  check(load().inventory.slots == rollback.inventory.slots &&
        load().presets == rollback.presets);
  {
    Statement disable(connection.Handle(),
                      "UPDATE Accounts SET IsDisabled = 1 WHERE AccountId = ?");
    std::array parameters{Number(owner)};
    disable.Execute(parameters);
  }
  rejects([&] { store.ChangeWeaponPreset(character, {0xFFFFFFFF, 0}, now); });
  check(!store.LoadSnapshot(owner, character.character_id, now));
  std::cout << "PASS: weapon preset persistence, " << checks << " checks.\n";
  return checks;
}
}  // namespace bubble_fighter::server::tests
