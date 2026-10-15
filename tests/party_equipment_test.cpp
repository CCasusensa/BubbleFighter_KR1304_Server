#include <algorithm>
#include <array>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "features/inventory/equipment_change.h"
#include "features/inventory/item_catalog.h"
#include "features/inventory/weapon_preset_state.h"
#include "features/rooms/room_map_catalog.h"
#include "protocol/inventory/equip_item_request.h"
#include "protocol/inventory/inventory_view_update_notification.h"
#include "protocol/inventory/unequip_item_request.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/rooms/create_room_request.h"
#include "protocol/rooms/room_ready_request.h"
#include "protocol/rooms/start_game_request.h"
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
    throw std::runtime_error("Party equipment line " +
                             std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected);
}
void Sequence(const std::vector<proto::TcpPacket>& packets,
              std::initializer_list<std::uint16_t> expected,
              std::source_location at = std::source_location::current()) {
  if (packets.size() != expected.size() ||
      !std::equal(packets.begin(), packets.end(), expected.begin(),
                  [](const auto& p, auto id) { return p.message_id == id; })) {
    std::cerr << "Party equipment sequence at " << at.line() << ":";
    for (const auto& p : packets) std::cerr << " " << p.message_id;
    std::cerr << "\n";
  }
  Check(packets.size() == expected.size(), at);
  auto it = expected.begin();
  for (const auto& p : packets) Check(p.message_id == *it++, at);
}
struct Characters final : bf::features::characters::CharacterStore {
  bf::features::characters::CharacterRecord record;
  std::optional<bf::features::characters::CharacterRecord> FindByAccount(
      std::uint64_t id) override {
    return id == record.account_id ? std::optional{record} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(
      bf::features::characters::CharacterRecord&) override {
    throw std::logic_error("Unexpected character create");
  }
};
struct Bag {
  inv::CharacterInventory inventory;
  inv::WeaponPresetState presets;
  int reads = 0, writes = 0, fault = 0;
};
struct Fixture {
  using Session = bf::sessions::PlayerSession;
  using Practice = bf::features::practice::PracticeSession;
  Practice::Time now{};
  std::shared_ptr<bf::features::rooms::RoomRegistry> rooms =
      bf::features::rooms::RoomRegistry::Create(4, [this] { return now; });
  std::shared_ptr<bf::features::lobby::PlazaRegistry> plazas =
      std::make_shared<bf::features::lobby::PlazaRegistry>();
  std::shared_ptr<bf::features::parties::PartyRegistry> parties =
      std::make_shared<bf::features::parties::PartyRegistry>(8);
  std::shared_ptr<bf::features::social::RoomInvitations> invitations =
      std::make_shared<bf::features::social::RoomInvitations>(
          rooms, bf::features::social::RoomInvitations::AllowsPair{}, parties,
          plazas);
  std::array<std::shared_ptr<Characters>, 2> stores;
  std::array<std::shared_ptr<Bag>, 2> bags;
  std::array<std::unique_ptr<Session>, 2> players;
  Fixture() {
    for (std::size_t i = 0; i < 2; ++i) {
      stores[i] = std::make_shared<Characters>();
      stores[i]->record = {100 + i, 200 + i, i == 0 ? u"Alice" : u"Bob",
                           i == 0 ? 27U : 10U, i == 0 ? 2U : 7U};
      bags[i] = std::make_shared<Bag>();
      auto bag = bags[i];
      const auto c = stores[i]->record;
      bag->inventory = {c.account_id, c.character_id, {}};
      for (auto id :
           {201005U, 202006U, 205033U, 207001U, 300000U + c.character_type}) {
        inv::InventorySlot slot;
        slot.item_id = id;
        slot.usage = inv::ItemCatalog::Find(id)->usage;
        slot.equipped = true;
        bag->inventory.slots.push_back(slot);
      }
      bag->presets.CaptureActive(bag->inventory);
      players[i] = std::make_unique<Session>(
          [c](std::string_view) { return std::optional{c.account_id}; },
          stores[i],
          [bag](const auto& member, auto observed) {
            ++bag->reads;
            auto data = bf::sessions::CreateMenuSnapshot(member, bag->inventory,
                                                         observed);
            data.presets = bag->presets.CreateNotification(
                member, bag->inventory, observed);
            if (bag->fault == 1) ++data.equipment.key;
            if (bag->fault == 2) ++data.inventory.account_id;
            if (bag->fault == 3) data.equipment.groups[0].resize(1025);
            if (bag->fault == 4) data.equipment.value = 1;
            return data;
          },
          [bag](const auto& member, auto item, bool equip, auto observed) {
            auto changed = inv::EquipmentChange::Prepare(member, bag->inventory,
                                                         item, equip, observed);
            bag->inventory = changed.inventory;
            bag->presets.CaptureActive(bag->inventory);
            changed.packets.push_back(
                bag->presets
                    .CreateNotification(member, bag->inventory, observed)
                    .Encode());
            ++bag->writes;
            return changed.packets;
          },
          Session::ShopCatalogProvider{}, Session::ShopPurchaseHandler{},
          Session::CashPickupHandler{},
          [bag](const auto& member, const auto& request, auto observed) {
            auto changed = inv::PrepareWeaponPresetChange(
                member, bag->inventory, bag->presets, request, observed);
            bag->inventory = changed.inventory;
            bag->presets = changed.presets;
            ++bag->writes;
            return changed.packets;
          },
          Session::ItemUseHandler{}, [this] { return now; },
          [] { return 12345U; }, rooms, plazas, nullptr, invitations, parties);
    }
  }
  proto::PartySupplementaryRecord View(
      std::size_t i, std::initializer_list<std::uint32_t> items) {
    proto::PartySupplementaryRecord result;
    result.key = static_cast<std::uint32_t>(stores[i]->record.character_id);
    result.value = 300000 + stores[i]->record.character_type;
    result.groups[0] = items;
    return result;
  }
  proto::PartySupplementaryRecord Default(std::size_t i) {
    return View(i, {201005, 202006, 205033, 207001,
                    300000 + stores[i]->record.character_type});
  }
  void Roster(const proto::TcpPacket& packet,
              const std::vector<proto::PartySupplementaryRecord>& expected) {
    const auto value = proto::UpdatePartyNotification::Decode(packet);
    Check(value.state == 2 && value.members.size() == expected.size());
    // The supplementary payload is a map keyed by character, not leader order.
    Check(value.supplementary_records.size() == expected.size());
    for (const auto& record : expected) {
      const auto found =
          std::find_if(value.supplementary_records.begin(),
                       value.supplementary_records.end(),
                       [&](const auto& x) { return x.key == record.key; });
      Check(found != value.supplementary_records.end());
      Check(*found == record);
    }
    for (std::size_t i = 0; i < expected.size(); ++i)
      Check(value.members[i].identity_value == expected[i].key);
  }
  void Login(std::size_t i) {
    auto& p = *players[i];
    proto::ClientToServerMessage0x0001 hello;
    hello.initial_value = 1256;
    p.Process(hello.Encode());
    proto::LoginRequest login;
    login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
    auto packets = p.Process(login.Encode());
    Sequence(packets, {3, 201});
    Check(proto::UpdatePartyNotification::Decode(packets[1])
              .supplementary_records.empty());
    packets = p.Process({521, {}});
    Sequence(packets, {105, 117, 806, 837, 802, 201});
    Roster(packets.back(), {Default(i)});
    Check(proto::InventoryViewUpdateNotification::Decode(packets[2]).view ==
          Default(i));
    const auto reads = bags[i]->reads;
    for (int n = 0; n < 10; ++n) Sequence(p.Poll(), {});
    Check(bags[i]->reads ==
          reads);  // Polling must not query persistence again.
  }
  void Join() {
    now += std::chrono::seconds(4);
    Sequence(players[0]->Process(proto::InvitationRequest{u"Bob", 1}.Encode()),
             {120});
    Sequence(players[1]->Poll(), {121});
    auto packets = players[1]->Process(
        proto::MoveToInviterRequest{u"Alice", 0, 0, 1}.Encode());
    Sequence(packets, {117, 201, 206});
    Roster(packets[1], {Default(0), Default(1)});
    packets = players[0]->Poll();
    Sequence(packets, {201, 206});
    Roster(packets[0], {Default(0), Default(1)});
  }
};
void MenuAndPlaza() {
  Fixture f;
  f.Login(0);
  f.Login(1);
  f.Join();
  auto packets =
      f.players[0]->Process(proto::UnequipItemRequest{202006}.Encode());
  Sequence(packets, {802, 806, 837, 201});
  const auto removed = f.View(0, {201005, 205033, 207001, 300027});
  Check(proto::InventoryViewUpdateNotification::Decode(packets[1]).view ==
        removed);
  f.Roster(packets.back(), {removed, f.Default(1)});
  auto peer = f.players[1]->Poll();
  Sequence(peer, {201});
  f.Roster(peer[0], {removed, f.Default(1)});
  Sequence(f.players[0]->Process(proto::UnequipItemRequest{202006}.Encode()),
           {802, 806, 837});
  Sequence(f.players[1]->Poll(), {});
  packets = f.players[0]->Process(proto::EquipItemRequest{202006}.Encode());
  Sequence(packets, {802, 806, 837, 201});
  f.Roster(packets.back(), {f.Default(0), f.Default(1)});
  Sequence(f.players[1]->Poll(), {201});
  Sequence(f.players[0]->Process({106, {}}), {117});
  Sequence(f.players[1]->Process({106, {}}), {117});
  Sequence(f.players[0]->Poll(), {503, 501});
  packets = f.players[1]->Process(
      proto::WeaponPresetChangeRequest{0xffffffff, 4}.Encode());
  Sequence(packets, {802, 806, 837, 836, 201});
  const auto preset = f.View(1, {201005, 207001, 300010});
  f.Roster(packets.back(), {f.Default(0), preset});
  peer = f.players[0]->Poll();
  Sequence(peer, {503, 201});
  f.Roster(peer.back(), {f.Default(0), preset});
  // Explicit inventory refresh picks up another trusted committed change.
  f.bags[1]->inventory.slots[0].equipped = false;
  packets = f.players[1]->Process({803, {}});
  Sequence(packets, {802, 837, 806, 201});
  f.Roster(packets.back(), {f.Default(0), f.View(1, {207001, 300010})});
  peer = f.players[0]->Poll();
  Sequence(peer, {201});
  f.Roster(peer[0], {f.Default(0), f.View(1, {207001, 300010})});
  // Scene changes retain equipment and do not broadcast an empty map.
  Sequence(f.players[1]->Process({105, {}}), {105, 117, 806, 837, 802});
  Sequence(f.players[0]->Poll(), {511, 501});
  packets = f.players[1]->Process(proto::LeavePartyRequest{}.Encode());
  Sequence(packets, {201, 204});
  f.Roster(packets[0], {f.View(1, {207001, 300010})});
  peer = f.players[0]->Poll();
  Sequence(peer, {201, 205});
  f.Roster(peer[0], {f.Default(0)});
}
void RoomAndBusy() {
  Fixture f;
  f.Login(0);
  f.Login(1);
  f.Join();
  f.players[0]->Process({106, {}});
  proto::CreateRoomRequest create;
  create.settings.text_at8 = u"Equipment";
  create.settings.configuration.short_at4 = 1;
  auto packets = f.players[0]->Process(create.Encode());
  Sequence(packets, {102, 117});
  f.now += std::chrono::seconds(4);
  Sequence(f.players[0]->Process(proto::InvitationRequest{u"Bob", 1}.Encode()),
           {120});
  Sequence(f.players[1]->Poll(), {121});
  Sequence(f.players[1]->Process(
               proto::MoveToInviterRequest{u"Alice", 0, 0, 1}.Encode()),
           {117});
  Sequence(f.players[0]->Poll(), {503});
  packets = f.players[1]->Process(proto::UnequipItemRequest{202006}.Encode());
  Sequence(packets, {802, 806, 837, 503, 201});
  const auto removed = f.View(1, {201005, 205033, 207001, 300010});
  f.Roster(packets.back(), {f.Default(0), removed});
  auto peer = f.players[0]->Poll();
  Sequence(peer, {503, 201});
  f.Roster(peer.back(), {f.Default(0), removed});
  f.players[1]->Process(proto::RoomReadyRequest{1, 0, 0}.Encode());
  f.players[0]->Poll();
  const auto& m = bf::features::rooms::RoomMapCatalog::Maps().front();
  proto::StartGameRequest start{0,
                                std::u16string(m.directory),
                                std::u16string(m.name),
                                std::u16string(m.key),
                                m.difficulty,
                                m.rules,
                                0};
  Sequence(f.players[0]->Process(start.Encode()), {585, 510});
  Sequence(f.players[1]->Poll(), {585, 510});
  const auto writes = f.bags[1]->writes;
  Sequence(f.players[1]->Process(proto::EquipItemRequest{202006}.Encode()),
           {509});
  Check(f.bags[1]->writes == writes);
  Sequence(f.players[0]->Poll(), {});
  Sequence(f.players[0]->Process({624, {1, 0, 0, 0, 0, 0, 0, 0}}), {620, 117});
  Sequence(f.players[1]->Poll(), {620, 117});
  packets = f.players[1]->Process(proto::LeavePartyRequest{}.Encode());
  Sequence(packets, {201, 204});
  f.Roster(packets[0], {removed});
  peer = f.players[0]->Poll();
  Sequence(peer, {201, 205});
  f.Roster(peer[0], {f.Default(0)});
}
void InvalidProvider() {
  for (int fault = 1; fault <= 3; ++fault) {
    Fixture f;
    f.Login(0);
    f.Login(1);
    f.Join();
    f.bags[0]->fault = fault;
    Rejects([&] { f.players[0]->Process({803, {}}); });
    Check(f.players[0]->State() == bf::sessions::PlayerSessionState::kRejected);
    auto packets = f.players[1]->Poll();
    Sequence(packets, {201, 203});
    f.Roster(packets[0], {f.Default(1)});
    Check(!f.parties->Capture(u"Alice"));
  }
  // Shared scene/inventory binding takes the model from the trusted character.
  Fixture bound;
  bound.bags[0]->fault = 4;
  bound.Login(0);
}
}  // namespace
int RunPartyEquipmentTests() {
  MenuAndPlaza();
  RoomAndBusy();
  InvalidProvider();
  std::cout << "Party equipment: " << checks << " checks passed\n";
  return checks;
}
