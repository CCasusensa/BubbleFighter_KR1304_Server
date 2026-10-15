#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "features/inventory/equipment_change.h"
#include "features/inventory/weapon_preset_state.h"
#include "protocol/inventory/weapon_preset_change_response.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/unresolved/client_to_server_message_0x0069.h"
#include "protocol/unresolved/client_to_server_message_0x0209.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bubble_fighter::server::tests {
namespace {
class PresetCharacters final : public features::characters::CharacterStore {
 public:
  features::characters::CharacterRecord record{17, 77, u"Preset", 10, 7};
  std::optional<features::characters::CharacterRecord> FindByAccount(
      std::uint64_t account) override {
    return account == record.account_id ? std::optional{record} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override {
    throw std::runtime_error("Unexpected name query.");
  }
  features::characters::CharacterCreateResult Create(
      features::characters::CharacterRecord&) override {
    throw std::runtime_error("Unexpected character creation.");
  }
};
}  // namespace

std::size_t RunWeaponPresetChangeTests(const std::filesystem::path& directory) {
  using namespace protocol;
  using namespace features::inventory;
  using namespace features::authentication;
  using namespace sessions;
  std::size_t checks = 0, rows = 0;
  const auto check = [&](bool ok) {
    ++checks;
    if (!ok) {
      throw std::runtime_error("Weapon preset check " + std::to_string(checks) +
                               " failed.");
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
  std::ifstream input(directory / "weapon_preset_packets.txt");
  if (!input) {
    throw std::runtime_error("Missing weapon preset native fixtures.");
  }
  for (std::string line; std::getline(input, line);) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    ++rows;
    std::istringstream stream(line);
    std::array<std::string, 5> fields;
    for (auto& field : fields) {
      if (!std::getline(stream, field, '|')) {
        throw std::runtime_error("Invalid preset fixture.");
      }
    }
    const bool request = fields[1] == "1";
    const auto index = static_cast<std::uint8_t>(std::stoul(fields[2]));
    const auto value = static_cast<std::uint32_t>(std::stoul(fields[3]));
    TcpPacket packet{request ? WeaponPresetChangeRequest::kMessageId
                             : WeaponPresetChangeResponse::kMessageId,
                     {}};
    for (std::size_t i = 0; i != fields[4].size(); i += 2) {
      packet.payload.push_back(static_cast<std::uint8_t>(
          std::stoul(fields[4].substr(i, 2), nullptr, 16)));
    }
    if (request) {
      const WeaponPresetChangeRequest expected{value, index};
      check(expected.Encode().payload == packet.payload);
      check(WeaponPresetChangeRequest::Decode(packet) == expected);
    } else {
      const WeaponPresetChangeResponse expected{index, value};
      check(expected.Encode().payload == packet.payload);
      check(WeaponPresetChangeResponse::Decode(packet) == expected);
    }
    const auto decode = [&](const TcpPacket& p) {
      if (request) {
        (void)WeaponPresetChangeRequest::Decode(p);
      } else {
        (void)WeaponPresetChangeResponse::Decode(p);
      }
    };
    for (std::size_t n = 0; n != packet.payload.size(); ++n) {
      rejects([&] {
        decode({packet.message_id,
                {packet.payload.begin(), packet.payload.begin() + n}});
      });
    }
    rejects([&] { decode({0, packet.payload}); });
    packet.payload.push_back(0);
    rejects([&] { decode(packet); });
  }
  check(rows == 14);
  for (unsigned index = 5; index != 256; ++index) {
    rejects([&] {
      WeaponPresetChangeRequest{0xFFFFFFFF, static_cast<std::uint8_t>(index)}
          .Encode();
    });
    rejects([&] {
      WeaponPresetChangeRequest::Decode(
          {832, {255, 255, 255, 255, static_cast<std::uint8_t>(index)}});
    });
  }

  constexpr std::uint64_t now = 1'800'000'000;
  auto characters = std::make_shared<PresetCharacters>();
  const auto character = characters->record;
  CharacterInventory inventory{77, 17, {}};
  for (const auto id : {201001U, 202001U, 203001U}) {
    InventorySlot slot;
    slot.item_id = id;
    slot.usage = InventoryUsage::kDuration;
    slot.quantity = 1;
    slot.equipped = id != 203001;
    slot.revision = 7;
    inventory.slots.push_back(slot);
  }
  WeaponPresetState state;
  state.CaptureActive(inventory);
  check(state.primary_items ==
        std::vector<std::uint32_t>({202001, 0, 0, 0, 0}));
  auto notification = state.CreateNotification(character, inventory, now);
  check(notification.player_id == 17 && notification.byte_at0 == 0 &&
        notification.records.size() == 5);
  check(notification.records[0] == std::array<std::uint32_t, 3>{202001, 0, 0} &&
        notification.bytes.empty());
  state.primary_items[4] = 203001;
  auto selected = PrepareWeaponPresetChange(character, inventory, state,
                                            {0xFFFFFFFF, 4}, now);
  check(selected.presets.active_index == 4 && selected.packets.size() == 4);
  check(selected.inventory.slots[0].equipped &&
        !selected.inventory.slots[1].equipped &&
        selected.inventory.slots[2].equipped);
  check(selected.inventory.slots[0] == inventory.slots[0] &&
        inventory.slots[1].equipped);
  check(selected.packets[0].message_id == 802 &&
        selected.packets[1].message_id == 806 &&
        selected.packets[2].message_id == 837 &&
        selected.packets[3].message_id == 836);
  check(WeaponPresetChangeResponse::Decode(selected.packets.back()) ==
        WeaponPresetChangeResponse{4, 0});
  const auto repeated = PrepareWeaponPresetChange(
      character, selected.inventory, selected.presets, {0xFFFFFFFF, 4}, now);
  check(repeated.inventory.slots == selected.inventory.slots &&
        repeated.presets == selected.presets);
  const auto restored = PrepareWeaponPresetChange(
      character, selected.inventory, selected.presets, {0xFFFFFFFF, 0}, now);
  check(restored.inventory.slots == inventory.slots);
  const auto empty = PrepareWeaponPresetChange(character, inventory, state,
                                               {0xFFFFFFFF, 1}, now);
  check(empty.inventory.slots[0].equipped &&
        !empty.inventory.slots[1].equipped &&
        !empty.inventory.slots[2].equipped);
  auto equipped =
      EquipmentChange::Prepare(character, empty.inventory, 203001, true, now);
  auto edited = empty.presets;
  edited.CaptureActive(equipped.inventory);
  check(edited.primary_items[0] == 202001 && edited.primary_items[1] == 203001);
  auto removed = EquipmentChange::Prepare(character, equipped.inventory, 203001,
                                          false, now);
  edited.CaptureActive(removed.inventory);
  check(edited.primary_items[1] == 0);
  auto expired = inventory;
  expired.slots[2].expires_at_unix_seconds = now;
  check(state.CreateNotification(character, expired, now).records[4][0] == 0);
  check(state.CreateNotification(character, expired, now - 1).records[4][0] ==
        203001);
  const auto expired_selection = PrepareWeaponPresetChange(
      character, expired, state, {0xFFFFFFFF, 4}, now);
  check(expired_selection.presets.primary_items[4] == 0 &&
        !expired_selection.inventory.slots[2].equipped);
  rejects([&] {
    PrepareWeaponPresetChange(character, inventory, state, {123, 0}, now);
  });
  for (int fault = 0; fault != 9; ++fault) {
    auto broken = state;
    auto bag = inventory;
    auto owner = character;
    switch (fault) {
      case 0:
        broken.primary_items.clear();
        break;
      case 1:
        broken.primary_items.push_back(0);
        break;
      case 2:
        broken.active_index = 5;
        break;
      case 3:
        broken.primary_items[1] = 201001;
        break;
      case 4:
        broken.primary_items[1] = 204001;
        break;  // real but not owned
      case 5:
        bag.account_id = 78;
        break;
      case 6:
        bag.character_id = 18;
        break;
      case 7:
        owner.statistics.level = 0;
        break;
      case 8:
        bag.slots[1].usage = InventoryUsage::kCount;
        break;
    }
    rejects([&] { broken.CreateNotification(owner, bag, now); });
  }
  auto conflict = inventory;
  conflict.slots[2].equipped = true;
  rejects([&] { state.CaptureActive(conflict); });
  auto locked = state;
  locked.primary_items.resize(1);
  rejects([&] {
    PrepareWeaponPresetChange(character, inventory, locked, {0xFFFFFFFF, 1},
                              now);
  });

  auto current_inventory = inventory;
  auto current_state = state;
  std::size_t calls = 0;
  const auto ticket = [](std::string_view) -> std::optional<std::uint64_t> {
    return 77;
  };
  const auto menu = [&](const auto& profile, auto observed) {
    auto data =
        sessions::CreateMenuSnapshot(profile, current_inventory, observed);
    data.presets =
        current_state.CreateNotification(profile, current_inventory, observed);
    return data;
  };
  const auto handler = [&](const auto& profile, const auto& request,
                           auto observed) {
    ++calls;
    auto result = PrepareWeaponPresetChange(profile, current_inventory,
                                            current_state, request, observed);
    current_inventory = result.inventory;
    current_state = result.presets;
    return result.packets;
  };
  const auto authenticate = [&](auto& session) {
    ClientToServerMessage0x0001 initial;
    initial.initial_value = 1256;
    LoginRequest login;
    login.nexon_passport = u"BFS1." + std::u16string(64, u'a');
    session.Process(initial.Encode());
    session.Process(login.Encode());
  };
  const auto initialize = [&](auto& session) {
    authenticate(session);
    session.Process(ClientToServerMessage0x0209{}.Encode());
  };
  PlayerSession session{ticket, characters, menu, {}, {}, {}, {}, handler};
  initialize(session);
  const auto packets =
      session.Process(WeaponPresetChangeRequest{0xFFFFFFFF, 4}.Encode());
  check(packets.size() == 4 && current_inventory.slots[2].equipped &&
        calls == 1);
  check(session.State() == PlayerSessionState::kMenuInitializationSent &&
        session.AccountId() == 77);
  session.Process(ClientToServerMessage0x006A{}.Encode());
  session.Process(WeaponPresetChangeRequest{0xFFFFFFFF, 0}.Encode());
  check(current_inventory.slots[1].equipped && calls == 2);
  const auto reload = session.Process(ClientToServerMessage0x0069{}.Encode());
  check(WeaponPresetUpdateNotification::Decode(reload[3]).records[4][0] ==
        203001);
  PlayerSession reconnect{ticket, characters, menu, {}, {}, {}, {}, handler};
  initialize(reconnect);
  check(calls == 2 && current_state.active_index == 0);
  for (int fault = 0; fault != 5; ++fault) {
    PlayerSession invalid{ticket, characters, menu, {}, {}, {}, {}, handler};
    if (fault == 1) {
      authenticate(invalid);
    } else if (fault >= 2) {
      initialize(invalid);
    }
    auto request = WeaponPresetChangeRequest{}.Encode();
    if (fault == 2) {
      request.payload.push_back(0);
    }
    if (fault == 3) {
      request = WeaponPresetChangeRequest{123, 0}.Encode();
    }
    if (fault == 4) {
      request.payload[4] = 5;
    }
    rejects([&] { invalid.Process(request); });
    check(calls == 2 && invalid.State() == PlayerSessionState::kRejected &&
          !invalid.AccountId());
  }
  PlayerSession missing{ticket, characters, menu};
  initialize(missing);
  rejects([&] { missing.Process(WeaponPresetChangeRequest{}.Encode()); });
  check(calls == 2);
  PlayerSession failing{
      ticket,
      characters,
      menu,
      {},
      {},
      {},
      {},
      [](const auto&, const auto&, auto) -> std::vector<TcpPacket> {
        throw std::runtime_error("Synthetic preset write failure.");
      }};
  initialize(failing);
  rejects([&] { failing.Process(WeaponPresetChangeRequest{}.Encode()); });
  check(failing.State() == PlayerSessionState::kRejected &&
        current_inventory.slots == inventory.slots);
  std::cout << "PASS: weapon preset codec, primary state and session, "
            << checks << " checks.\n";
  return checks;
}
}  // namespace bubble_fighter::server::tests
