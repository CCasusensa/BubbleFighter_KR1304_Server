#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
#include <thread>

#include "protocol/clans/clan_presence_report.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/unresolved/client_to_server_message_0x006b.h"
#include "sessions/clan_session.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace {
namespace bf = bubble_fighter::server;
namespace c = bf::features::clans;
namespace p = bf::protocol;
using Auth = c::ClanAuthentication;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Clan presence line " + std::to_string(at.line()));
}
template <class F>
void Rejects(F f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception&) {
    failed = true;
  }
  Check(failed);
}
auto Profiles(const c::ClanIdentity&) {
  return std::optional{c::ClanSnapshot{}};
}
void Registry() {
  auto auth = std::make_shared<Auth>();
  auto first = auth->BindGame({1, 10});
  auto second = auth->BindGame({2, 20});
  const std::array<std::uint32_t, 5> ids{10, 20, 10, 0, 99};
  Check(auth->ReadScenes(ids) == std::vector<std::uint32_t>(5));
  auto binding = auth->Consume(auth->Issue(first, u"First"));
  auth->UpdateScene(first, {60, 3});
  auto epoch = binding.PlazaEpoch();
  Check(epoch.has_value() && binding.Scene() == c::ClanGameScene{60, 3});
  auth->UpdateScene(first, {60, 3});
  Check(binding.PlazaEpoch() == epoch);
  auth->UpdateScene(first, {20, 3});
  Check(binding.PlazaEpoch() != epoch && binding.PlazaEpoch().has_value());
  auth->UpdateScene(second, {60, 2});
  Check(auth->ReadScenes(ids) == std::vector<std::uint32_t>({3, 2, 3, 0, 0}));
  for (const auto scene :
       {c::ClanGameScene{80, 3}, {80, 6}, {60, 4}, {60, 1}, {60, 2}, {60, 0}}) {
    auth->UpdateScene(first, scene);
    Check(!binding.PlazaEpoch() && binding.Scene() == scene);
  }
  Rejects([&] { auth->UpdateScene(first, {50, 3}); });
  Rejects([&] { auth->UpdateScene(first, {60, 5}); });
  Rejects([&] { auth->UpdateScene({}, {60, 3}); });
  auto other = std::make_shared<Auth>();
  Rejects([&] { other->UpdateScene(first, {60, 3}); });
  Check(binding.Scene() == c::ClanGameScene{60, 0});
  auth->UpdateScene(first, {60, 1});
  auto replacement = auth->Consume(auth->Issue(first, u"First"));
  Check(!binding.Scene() && replacement.Scene() == c::ClanGameScene{60, 1});
  replacement = {};
  Check(auth->ReadScenes(ids)[0] == 1);  // Main survives auxiliary closure.
  std::atomic<bool> good = true;
  std::thread writer([&] {
    for (int i = 0; i < 400; ++i)
      auth->UpdateScene(first, {60, i % 2 ? 1U : 3U});
  });
  std::thread reader([&] {
    for (int i = 0; i < 400; ++i) {
      const auto read = auth->ReadScenes(ids);
      if (read[0] != read[2] || read[1] != 2) good = false;
    }
  });
  writer.join();
  reader.join();
  Check(good);
  first.reset();
  Check(auth->ReadScenes(ids) == std::vector<std::uint32_t>({0, 2, 0, 0, 0}));
  auto relogin = auth->BindGame({1, 10});
  Check(auth->ReadScenes(ids)[0] == 0);
}
void Session() {
  auto auth = std::make_shared<Auth>();
  auto owner = auth->BindGame({1, 10});
  auto friend_game = auth->BindGame({2, 20});
  auth->UpdateScene(owner, {60, 3});
  auth->UpdateScene(friend_game, {60, 2});
  int reads = 0;
  const auto make = [&] {
    return bf::sessions::ClanSession(
        auth, Profiles, Auth::Clock::now, {}, {},
        [&](const auto&, auto) -> std::optional<std::vector<p::ClanPacket>> {
          ++reads;
          return std::vector{p::ClanRosterResponse{
              {{10, u"First", 999, 0, 101},
               {20, u"Second", 999, 0, 100},
               {30, u"Absent", 999, 0,
                100}}}.Encode()};
        });
  };
  auto session = make();
  session.Process(p::ClanLoginRequest{auth->Issue(owner, u"First")}.Encode());
  auto observer = auth->Find(10);
  const auto epoch = observer.PlazaEpoch();
  for (const auto report :
       {p::ClanPresenceReport{1, 6}, {0xffffffff, 0xffffffff}, {45, 0}}) {
    const auto result = session.Process(report.Encode());
    Check(!result.close_connection && result.responses.empty());
    Check(observer.Scene() == c::ClanGameScene{60, 3} &&
          observer.PlazaEpoch() == epoch && reads == 0);
  }
  const auto roster = [&] {
    const auto result = session.Process(p::ClanRosterRequest{}.Encode());
    Check(!result.close_connection && result.responses.size() == 1);
    const auto rows =
        p::ClanRosterResponse::Decode(result.responses[0]).members;
    Check(rows.size() == 3 && rows[0].membership_status == 101 &&
          rows[0].value_at28 == 0);
    return rows;
  };
  auto rows = roster();
  Check(rows[0].value_at36 == 3 && rows[1].value_at36 == 2 &&
        rows[2].value_at36 == 0);
  friend_game.reset();
  rows = roster();
  Check(rows[1].value_at36 == 0);
  auth->UpdateScene(owner, {60, 0});
  rows = roster();
  Check(rows[0].value_at36 == 0);
  for (const std::size_t size : {0U, 7U, 9U}) {
    auto malformed = make();
    malformed.Process(
        p::ClanLoginRequest{auth->Issue(owner, u"First")}.Encode());
    Rejects(
        [&] { malformed.Process({1045, std::vector<std::uint8_t>(size)}); });
    Check(malformed.Poll().close_connection);
  }
  auto early = make();
  Rejects([&] { early.Process(p::ClanPresenceReport{1, 3}.Encode()); });
  auto stale = make();
  stale.Process(p::ClanLoginRequest{auth->Issue(owner, u"First")}.Encode());
  owner.reset();
  const auto denied = stale.Process(p::ClanPresenceReport{1, 3}.Encode());
  Check(denied.close_connection && denied.responses.empty());
}
struct Characters final : bf::features::characters::CharacterStore {
  bf::features::characters::CharacterRecord record{10, 1, u"First", 27, 2};
  std::optional<bf::features::characters::CharacterRecord> FindByAccount(
      std::uint64_t id) override {
    return id == 1 ? std::optional{record} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(
      bf::features::characters::CharacterRecord&) override {
    throw std::logic_error("Unexpected create");
  }
};
void MainScenes() {
  using Player = bf::sessions::PlayerSession;
  using Practice = bf::features::practice::PracticeSession;
  auto auth = std::make_shared<Auth>();
  auto characters = std::make_shared<Characters>();
  auto parties = std::make_shared<bf::features::parties::PartyRegistry>(8);
  std::istringstream config("[20]\nMinimumLevel=1\nMaximumLevel=20\n");
  auto worlds = std::make_shared<bf::features::channels::ChannelWorlds>(
      bf::features::channels::ChannelRules::Read(config), parties);
  auto world = worlds->Find(60);
  auto player = std::make_unique<Player>(
      [](auto) { return std::optional<std::uint64_t>{1}; }, characters,
      [](const auto& character, auto observed) {
        return bf::sessions::CreateMenuSnapshot(
            character, {character.account_id, character.character_id, {}},
            observed);
      },
      Player::EquipmentChangeHandler{}, Player::ShopCatalogProvider{},
      Player::ShopPurchaseHandler{}, Player::CashPickupHandler{},
      Player::WeaponPresetHandler{}, Player::ItemUseHandler{},
      Practice::Clock::now, [] { return 12345U; }, world->rooms, world->plazas,
      nullptr, world->invitations, parties, world->transfers, worlds, auth);
  p::ClientToServerMessage0x0001 initial;
  initial.initial_value = 1256;
  player->Process(initial.Encode());
  p::LoginRequest login;
  login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
  player->Process(login.Encode());
  const auto credential =
      p::ClanPassportResponse::Decode(player->Process({25, {}}).at(0))
          .credentials;
  auto binding = auth->Consume(credential);
  Check(binding.Scene() == c::ClanGameScene{60, 0});
  player->Process({521, {}});
  Check(binding.Scene() == c::ClanGameScene{60, 1});
  player->Process({106, {}});
  Check(binding.Scene() == c::ClanGameScene{60, 3});
  Check(binding.PlazaEpoch().has_value());
  Check(player->BeginConnectionHandoff(20));
  Check(binding.Scene() == c::ClanGameScene{60, 0} && !binding.PlazaEpoch());
  player->EndConnectionHandoff();
  Check(binding.Scene() == c::ClanGameScene{60, 3});
  Check(player->BeginConnectionHandoff(20));
  player->CompleteConnectionHandoff();
  Check(binding.Scene() == c::ClanGameScene{20, 3});
  Check(player->BeginConnectionHandoff(20));
  Check(binding.Scene() == c::ClanGameScene{20, 0});
  player->CompleteConnectionHandoff();
  Check(binding.Scene() == c::ClanGameScene{20, 3});
  p::CreateRoomRequest room;
  room.settings.text_at8 = u"Presence";
  room.settings.configuration.short_at4 = 1;
  player->Process(room.Encode());
  Check(binding.Scene() == c::ClanGameScene{20, 2} && !binding.PlazaEpoch());
  player->Process({404, {}});
  Check(binding.Scene() == c::ClanGameScene{20, 3});
  player->Process({105, {}});
  Check(binding.Scene() == c::ClanGameScene{20, 1});
  p::ClientToServerMessage0x006B practice;
  practice.rules = {3000,   6000,   4000,   11000,  45000, 142000, 20,
                    240000, 241000, 240000, 240000, 15000, 15000};
  practice.rule_flags = 16;
  practice.directory = practice.map_key = u"rel_Village07_Easy02";
  practice.map_name = u"泡泡村小河边休闲区";
  practice.difficulty = 1;
  player->Process(practice.Encode());
  Check(binding.Scene() == c::ClanGameScene{20, 2});
  player->Poll();
  Check(binding.Scene() == c::ClanGameScene{20, 2});
  Rejects([&] { player->Process({25, {1}}); });
  Check(!binding.Scene() &&
        auth->ReadScenes(std::array<std::uint32_t, 1>{10})[0] == 0);
}
}  // namespace
int RunClanPresenceTests(const std::filesystem::path& directory) {
  checks = 0;
  std::ifstream input(directory / "clan_presence_packets.txt");
  Check(input.good());
  std::string label, hex;
  std::uint16_t mid = 0;
  int fixtures = 0;
  while (input >> label >> mid >> hex) {
    p::ClanPacket packet{mid, {}};
    for (std::size_t i = 0; i < hex.size(); i += 2)
      packet.payload.push_back(
          static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    Check(p::ClanPresenceReport::Decode(packet).Encode() == packet);
    auto bad = packet;
    ++bad.message_id;
    Rejects([&] { p::ClanPresenceReport::Decode(bad); });
    for (std::size_t n = 0; n < 8; ++n) {
      bad = packet;
      bad.payload.resize(n);
      Rejects([&] { p::ClanPresenceReport::Decode(bad); });
    }
    bad = packet;
    bad.payload.push_back(0);
    Rejects([&] { p::ClanPresenceReport::Decode(bad); });
    ++fixtures;
  }
  Check(fixtures == 8);
  Registry();
  Session();
  MainScenes();
  std::cout << "Clan presence: " << checks << " checks passed.\n";
  return checks;
}
