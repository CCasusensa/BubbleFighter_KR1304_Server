#include "features/channels/channel_worlds.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <source_location>
#include <sstream>
#include <stdexcept>

#include "features/rooms/room_map_catalog.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/rooms/room_list_request.h"
#include "protocol/rooms/room_list_response.h"
#include "protocol/rooms/room_ready_request.h"
#include "protocol/rooms/start_game_request.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bf = bubble_fighter::server;
namespace ch = bf::features::channels;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Channel worlds line " +
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
ch::ChannelRules Rules(const std::string& text) {
  std::istringstream input(text);
  return ch::ChannelRules::Read(input);
}
struct Characters final : bf::features::characters::CharacterStore {
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
struct Fixture {
  using Session = bf::sessions::PlayerSession;
  using Time = bf::features::practice::PracticeSession::Time;
  Time now{};
  std::shared_ptr<bf::features::parties::PartyRegistry> parties =
      std::make_shared<bf::features::parties::PartyRegistry>(8);
  std::shared_ptr<ch::ChannelWorlds> worlds = std::make_shared<
      ch::ChannelWorlds>(
      Rules(
          "[20]\nMinimumLevel=1\nMaximumLevel=20\n[70]\nMinimumLevel=21\nMaximumLevel=260\n"),
      parties);
  std::array<std::shared_ptr<Characters>, 3> stores;
  std::array<std::unique_ptr<Session>, 3> players;
  bool bad_equipment = false;
  Fixture() {
    const std::array<std::u16string, 3> names{u"Alice", u"Bob", u"Carol"};
    auto world = worlds->Find(60);
    for (std::size_t i = 0; i < players.size(); ++i) {
      stores[i] = std::make_shared<Characters>();
      stores[i]->record = {100 + i, 200 + i, names[i], 27, 2};
      auto store = stores[i];
      players[i] = std::make_unique<Session>(
          [store](std::string_view) {
            return std::optional{store->record.account_id};
          },
          store,
          [this](const auto& c, auto observed) {
            auto result = bf::sessions::CreateMenuSnapshot(
                c, {c.account_id, c.character_id, {}}, observed);
            if (bad_equipment) ++result.equipment.key;
            return result;
          },
          Session::EquipmentChangeHandler{}, Session::ShopCatalogProvider{},
          Session::ShopPurchaseHandler{}, Session::CashPickupHandler{},
          Session::WeaponPresetHandler{}, Session::ItemUseHandler{},
          [this] { return now; }, [] { return 12345U; }, world->rooms,
          world->plazas, nullptr, world->invitations, parties, world->transfers,
          worlds);
      proto::ClientToServerMessage0x0001 hello;
      hello.initial_value = 1256;
      players[i]->Process(hello.Encode());
      proto::LoginRequest login;
      login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
      players[i]->Process(login.Encode());
      players[i]->Process({521, {}});
      players[i]->Process({106, {}});
    }
    Drain();
  }
  void Drain() {
    for (auto& p : players) p->Poll();
  }
  void Move(std::size_t i, std::uint32_t category) {
    Check(players[i]->BeginConnectionHandoff(category));
    const auto packets = players[i]->CompleteConnectionHandoff();
    Check(players[i]->ChannelCategory() == category);
    Check(packets.size() == 1 && packets[0].message_id == 117);
    Check(proto::GotoRoomNotification::Decode(packets[0])
              .descriptor.session_type == 3);
  }
  std::uint16_t Create(std::size_t i, const std::u16string& title) {
    proto::CreateRoomRequest request;
    request.settings.text_at8 = title;
    request.settings.configuration.short_at4 = 1;
    auto response = players[i]->Process(request.Encode());
    const auto found =
        std::find_if(response.begin(), response.end(),
                     [](const auto& p) { return p.message_id == 117; });
    Check(found != response.end());
    return proto::GotoRoomNotification::Decode(*found).descriptor.short_at0;
  }
  proto::RoomListResponse List(std::size_t i) {
    auto response = players[i]->Process(proto::RoomListRequest{}.Encode());
    const auto found =
        std::find_if(response.begin(), response.end(),
                     [](const auto& p) { return p.message_id == 101; });
    Check(found != response.end());
    return proto::RoomListResponse::Decode(*found);
  }
};
void Configuration() {
  Check(ch::ChannelRules{}.Categories().size() == 1);
  Check(Rules("# no optional categories\n").Categories().contains(60));
  for (const std::string text :
       {"[60]\n", "[80]\n", "[0]\n", "[-1]\n", "[20]\n", "MinimumLevel=1\n",
        "[20]\nMinimumLevel=1\n", "[20]\nMinimumLevel=0\nMaximumLevel=20\n",
        "[70]\nMinimumLevel=261\nMaximumLevel=260\n",
        "[20]\nMinimumLevel=21\nMaximumLevel=20\n",
        "[20]\nMinimumLevel=1\nMinimumLevel=2\nMaximumLevel=20\n",
        "[20]\nMinimumLevel=1\nMaximumLevel=20\n[20]\n",
        "[20]\nMinimumLevel=x\nMaximumLevel=20\n",
        "[20]\nUnknown=1\nMaximumLevel=20\n",
        "[20]\nMinimumLevel=1x\nMaximumLevel=20\n"})
    Rejects([&] { Rules(text); });
  Rejects([&] { Rules(std::string(16385, ' ')); });
  Fixture f;
  Check(!f.worlds->Find(80) && !f.worlds->Find(0));
  for (const auto level : {0U, 1U, 20U, 21U, 260U, 261U}) {
    auto c = f.stores[0]->record;
    c.statistics.level = level;
    Check(f.worlds->Find(20)->Allows(c) == (level >= 1 && level <= 20));
    Check(f.worlds->Find(70)->Allows(c) == (level >= 21 && level <= 260));
  }
  Check(f.worlds->Find(20)->rooms != f.worlds->Find(60)->rooms);
  Check(f.worlds->Find(20)->plazas != f.worlds->Find(60)->plazas);
  Check(f.worlds->Find(20)->invitations != f.worlds->Find(60)->invitations);
}
void IsolatedTravel() {
  Fixture f;
  Check(!f.players[0]->BeginConnectionHandoff(70));
  Check(!f.players[0]->BeginConnectionHandoff(80));
  f.Move(0, 20);
  Check(!f.worlds->Find(60)->plazas->Capture(200));
  Check(f.worlds->Find(20)->plazas->Capture(200).has_value());
  const auto beginner = f.Create(0, u"BeginnerRoom");
  f.Drain();
  Check(f.List(1).rooms.empty());
  const auto free = f.Create(1, u"FreeRoom");
  f.Drain();
  Check(beginner ==
        free);  // IDs may overlap, but commands resolve in the current world.
  Check(f.List(2).rooms.size() == 1 &&
        f.List(2).rooms[0].text_at8 == u"FreeRoom");
  f.players[2]->Process(proto::JoinRoomRequest{free, {}}.Encode());
  f.Drain();
  Check(f.worlds->Find(20)->rooms->Find(beginner)->occupants.size() == 1);
  Check(f.worlds->Find(60)->rooms->Find(free)->occupants.size() == 2);
  f.Move(0, 60);
  f.Drain();
  Check(!f.worlds->Find(20)->rooms->Find(beginner));
  Check(f.worlds->Find(60)->rooms->Find(free)->occupants.size() == 2);
  f.stores[0]->record.statistics.level = 21;
  f.Move(0, 70);
  f.Drain();
  Check(f.players[0]->Character()->statistics.level == 21);
  Check(!f.players[0]->BeginConnectionHandoff(20));
  Check(f.players[0]->BeginConnectionHandoff(70));
  Check(f.players[0]->CompleteConnectionHandoff().empty());
  Check(f.players[0]->ChannelCategory() == 70);
  f.Move(0, 60);
}
void RollbackAndParty() {
  Fixture f;
  for (int attempt = 0; attempt < 3; ++attempt) {
    Check(f.players[0]->BeginConnectionHandoff(20));
    Check(!f.worlds->Find(20)->plazas->Capture(200));
    Check(f.worlds->Find(60)->plazas->Capture(200).has_value());
    f.players[0]->EndConnectionHandoff();
    Check(f.players[0]->ChannelCategory() == 60);
  }
  f.bad_equipment = true;
  Rejects([&] { f.players[0]->BeginConnectionHandoff(20); });
  Check(f.players[0]->ChannelCategory() == 60 &&
        !f.worlds->Find(20)->plazas->Capture(200));
  f.bad_equipment = false;
  f.now += std::chrono::seconds(4);
  f.players[0]->Process(proto::InvitationRequest{u"Bob", 1}.Encode());
  f.players[1]->Poll();
  f.players[1]->Process(
      proto::MoveToInviterRequest{u"Alice", 0, 0, 1}.Encode());
  f.Drain();
  Check(!f.players[0]->BeginConnectionHandoff(20));
  Check(!f.players[1]->BeginConnectionHandoff(20));
  Check(f.players[0]->ChannelCategory() == 60 &&
        f.players[1]->ChannelCategory() == 60);
  Check(f.players[0]->BeginConnectionHandoff(60));
  f.players[0]->EndConnectionHandoff();
}
void LoadingBoundary() {
  Fixture f;
  const auto id = f.Create(0, u"StartRace");
  f.Drain();
  f.players[1]->Process(proto::JoinRoomRequest{id, {}}.Encode());
  f.Drain();
  f.players[1]->Process(proto::RoomReadyRequest{1, 0, 0}.Encode());
  f.Drain();
  Check(f.players[1]->BeginConnectionHandoff(20));
  const auto& m = bf::features::rooms::RoomMapCatalog::Maps().front();
  proto::StartGameRequest start{0,
                                std::u16string(m.directory),
                                std::u16string(m.name),
                                std::u16string(m.key),
                                m.difficulty,
                                m.rules,
                                0};
  f.players[0]->Process(start.Encode());
  Check(f.worlds->Find(60)->rooms->Find(id)->state ==
        bf::features::rooms::RoomState::kLoading);
  Rejects([&] { f.players[1]->CompleteConnectionHandoff(); });
  Check(!f.worlds->Find(20)->plazas->Capture(201));
  Check(f.worlds->Find(60)->rooms->Find(id)->occupants.size() == 2);
  f.players[1]->EndConnectionHandoff();
  Check(!f.players[0]->BeginConnectionHandoff(20));
  Check(f.players[0]->BeginConnectionHandoff(60));
  Check(f.players[0]->CompleteConnectionHandoff().empty());
}
void SharedSettlements() {
  namespace rooms = bf::features::rooms;
  auto parties = std::make_shared<bf::features::parties::PartyRegistry>(8);
  int calls = 0;
  ch::ChannelWorlds worlds(
      Rules("[20]\nMinimumLevel=1\nMaximumLevel=20\n"), parties, {},
      [&](const rooms::RoomMatchRequest& request) {
        ++calls;
        rooms::RoomMatchResult result;
        for (const auto& member : request.members) {
          bf::features::characters::CharacterStatistics before;
          bf::features::characters::CharacterRecord c{
              member.character_id, member.account_id, u"Alice", 27, 2};
          c.statistics = request.rules.Apply(before, member.scores, false);
          result.rewards.push_back({member.slot, c, before});
        }
        return result;
      });
  rooms::RoomMatchRequest request;
  request.match_id[0] = 1;
  request.duration_milliseconds = 100;
  request.members.push_back({0, 200, 100, 1, {}});
  worlds.Find(20)->rooms->RecoverSettlement(request);
  worlds.Find(60)->rooms->RecoverSettlement(request);
  Check(worlds.Find(60)->rooms->PendingSettlementCount() == 1);
  request.match_id[0] = 2;
  worlds.Find(60)->rooms->RecoverSettlement(request);
  Check(worlds.Find(20)->rooms->PendingSettlementCount() == 2);
  auto changed = request;
  ++changed.duration_milliseconds;
  Rejects([&] { worlds.Find(20)->rooms->RecoverSettlement(changed); });
  worlds.Find(60)->rooms->RetrySettlements();
  Check(calls == 2 && worlds.Find(20)->rooms->PendingSettlementCount() == 0);
  worlds.Find(20)->rooms->RetrySettlements();
  Check(calls == 2);
}
}  // namespace
int RunChannelWorldsTests() {
  LoadingBoundary();
  SharedSettlements();
  Configuration();
  IsolatedTravel();
  RollbackAndParty();
  std::cout << "Channel worlds: " << checks << " checks passed\n";
  return checks;
}
