#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>

#include "features/inventory/item_catalog.h"
#include "features/inventory/weapon_preset_state.h"
#include "features/rooms/room_map_catalog.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/rooms/create_room_request.h"
#include "protocol/rooms/join_room_request.h"
#include "protocol/rooms/room_ready_request.h"
#include "protocol/rooms/room_slot_update_notification.h"
#include "protocol/unresolved/client_to_server_message_0x0209.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bf = bubble_fighter::server;
namespace inv = bf::features::inventory;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Waiting-room preset check at " +
                             std::to_string(at.line()));
}
struct CharacterStore final : bf::features::characters::CharacterStore {
  bf::features::characters::CharacterRecord record;
  std::optional<bf::features::characters::CharacterRecord> FindByAccount(
      std::uint64_t account) override {
    return account == record.account_id ? std::optional{record} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(
      bf::features::characters::CharacterRecord&) override {
    throw std::logic_error("Unexpected character creation");
  }
};
struct Bag {
  inv::CharacterInventory inventory;
  inv::WeaponPresetState presets;
  int writes = 0;
};
}  // namespace
int RunEquipmentPresetRoomTests() {
  auto rooms = bf::features::rooms::RoomRegistry::Create(8);
  auto host_bag = std::make_shared<Bag>();
  auto guest_bag = std::make_shared<Bag>();
  const auto player = [&](std::uint64_t account, std::uint64_t character,
                          std::u16string name,
                          const std::shared_ptr<Bag>& bag) {
    auto store = std::make_shared<CharacterStore>();
    store->record = {character, account, std::move(name), 10, 7};
    bag->inventory = {account, character, {}};
    for (auto id : {202001U, 205033U, 100431U, 201001U, 207001U}) {
      inv::InventorySlot slot;
      slot.item_id = id;
      slot.usage = inv::ItemCatalog::Find(id)->usage;
      slot.quantity = 3;
      slot.equipped = true;
      bag->inventory.slots.push_back(slot);
    }
    bag->presets.CaptureActive(bag->inventory);
    return std::make_unique<bf::sessions::PlayerSession>(
        [account](std::string_view) { return std::optional{account}; }, store,
        [bag](const auto& member, auto now) {
          auto data =
              bf::sessions::CreateMenuSnapshot(member, bag->inventory, now);
          data.presets =
              bag->presets.CreateNotification(member, bag->inventory, now);
          return data;
        },
        bf::sessions::PlayerSession::EquipmentChangeHandler{},
        bf::sessions::PlayerSession::ShopCatalogProvider{},
        bf::sessions::PlayerSession::ShopPurchaseHandler{},
        bf::sessions::PlayerSession::CashPickupHandler{},
        [bag](const auto& member, const auto& request, auto now) {
          auto result = inv::PrepareWeaponPresetChange(
              member, bag->inventory, bag->presets, request, now);
          bag->inventory = result.inventory;
          bag->presets = result.presets;
          ++bag->writes;
          return result.packets;
        },
        bf::sessions::PlayerSession::ItemUseHandler{},
        bf::features::practice::PracticeSession::Clock::now,
        bf::features::practice::PracticeSession::CreateRandomSeed, rooms);
  };
  auto host = player(77, 17, u"PresetHost", host_bag);
  auto guest = player(78, 18, u"PresetGuest", guest_bag);
  const auto login = [](auto& session) {
    proto::ClientToServerMessage0x0001 initial;
    initial.initial_value = 1256;
    session.Process(initial.Encode());
    proto::LoginRequest login;
    login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
    session.Process(login.Encode());
    session.Process(proto::ClientToServerMessage0x0209{}.Encode());
    session.Process({106, {}});
  };
  login(*host);
  login(*guest);
  proto::CreateRoomRequest create;
  create.settings.text_at8 = u"Loadouts";
  create.settings.configuration.short_at4 = 1;
  const auto created = host->Process(create.Encode());
  const auto id =
      proto::GotoRoomNotification::Decode(created.at(1)).descriptor.short_at0;
  guest->Process(proto::JoinRoomRequest{id, {}}.Encode());
  host->Poll();
  guest->Process(proto::RoomReadyRequest{1, 0, 0}.Encode());
  host->Poll();
  Check(rooms->Find(id)->FindSlot(1)->member.booleans_at96[0] == 1);
  const auto changed =
      guest->Process(proto::WeaponPresetChangeRequest{0xffffffff, 4}.Encode());
  Check(changed.size() == 5 && changed.back().message_id == 503);
  for (std::size_t i = 0; i < 4; ++i)
    Check(changed[i].message_id ==
          std::array<std::uint16_t, 4>{802, 806, 837, 836}[i]);
  Check(proto::WeaponPresetUpdateNotification::Decode(changed[2]).records[4] ==
        std::array<std::uint32_t, 3>{0, 0, 0});
  const auto broadcast = host->Poll();
  Check(broadcast.size() == 1 && broadcast[0].message_id == 503);
  const auto update = proto::RoomSlotUpdateNotification::Decode(broadcast[0]);
  Check(update.slot == 1 && update.member.booleans_at96[0] == 0);
  Check(rooms->Find(id)->FindSlot(1)->equipment ==
        inv::EquipmentView::Create(*guest->Character(), guest_bag->inventory,
                                   1800000000));
  for (std::size_t i = 0; i < 3; ++i)
    Check(!guest_bag->inventory.slots[i].equipped);
  Check(guest_bag->inventory.slots[3].equipped &&
        guest_bag->inventory.slots[4].equipped);
  Check(guest->Process(proto::WeaponPresetChangeRequest{0xffffffff, 4}.Encode())
            .size() == 4);
  Check(host->Poll().empty());
  guest->Process(proto::RoomReadyRequest{1, 0, 0}.Encode());
  host->Poll();
  const auto& map = bf::features::rooms::RoomMapCatalog::Maps().front();
  proto::StartGameRequest start{0,
                                std::u16string(map.directory),
                                std::u16string(map.name),
                                std::u16string(map.key),
                                map.difficulty,
                                map.rules,
                                0};
  Check(host->Process(start.Encode()).back().message_id == 510);
  guest->Poll();
  const auto before = guest_bag->writes;
  const auto refused =
      guest->Process(proto::WeaponPresetChangeRequest{0xffffffff, 0}.Encode());
  Check(refused.size() == 1 && refused[0].message_id == 509);
  Check(guest_bag->writes == before && guest_bag->presets.active_index == 4);
  Check(guest->State() == bf::sessions::PlayerSessionState::kInRoom);
  host->Process({624, {1, 0, 0, 0, 0, 0, 0, 0}});
  guest->Poll();
  Check(guest->Process(proto::WeaponPresetChangeRequest{0xffffffff, 0}.Encode())
            .size() == 5);
  for (std::size_t i = 0; i < 3; ++i)
    Check(guest_bag->inventory.slots[i].equipped);
  std::cout << checks << " waiting-room equipment preset checks passed\n";
  return checks;
}
