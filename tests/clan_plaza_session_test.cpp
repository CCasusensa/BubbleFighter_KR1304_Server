#include <algorithm>
#include <array>
#include <iostream>
#include <source_location>
#include <sstream>

#include "features/channels/channel_worlds.h"
#include "protocol/clans/clan_plaza_request.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace {
namespace bf = bubble_fighter::server;
namespace c = bf::features::clans;
namespace ch = bf::features::channels;
namespace party = bf::features::parties;
namespace p = bf::protocol;
using Character = bf::features::characters::CharacterRecord;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Clan plaza session line " +
                             std::to_string(at.line()));
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
bool Has(const std::vector<p::TcpPacket>& packets, std::uint16_t id) {
  return std::any_of(packets.begin(), packets.end(),
                     [&](const auto& v) { return v.message_id == id; });
}
struct Characters final : bf::features::characters::CharacterStore {
  Character record;
  std::optional<Character> FindByAccount(std::uint64_t id) override {
    return id == record.account_id ? std::optional{record} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(Character&) override {
    throw std::logic_error("Unexpected create");
  }
};
struct Fixture {
  using Player = bf::sessions::PlayerSession;
  bf::features::practice::PracticeSession::Time now{};
  std::shared_ptr<c::ClanAuthentication> auth =
      std::make_shared<c::ClanAuthentication>();
  std::shared_ptr<party::PartyRegistry> parties =
      std::make_shared<party::PartyRegistry>(8);
  std::shared_ptr<ch::ChannelWorlds> worlds;
  std::array<std::shared_ptr<Characters>, 3> stores;
  std::array<std::unique_ptr<Player>, 3> players;
  std::array<c::ClanPlazaMember, 3> rows;
  std::array<std::vector<p::TcpPacket>, 3> output;
  int reads = 0, fault = -1;
  explicit Fixture(bool enable_clan_channel = true) {
    auto source = [this](auto ids) {
      ++reads;
      std::vector<c::ClanPlazaMember> result;
      for (const auto& id : ids) {
        auto found = std::find_if(
            rows.begin(), rows.end(),
            [&](const auto& row) { return row.member.identity == id; });
        if (found == rows.end())
          return std::optional<std::vector<c::ClanPlazaMember>>{};
        result.push_back(*found);
      }
      return std::optional{result};
    };
    auto gate = std::make_shared<c::ClanPlazaAccess>(auth, source);
    auto channel =
        std::make_shared<c::ClanChannelAccess>(auth, [source](auto ids) {
          std::vector<c::ClanChannelMember> result;
          const auto rows = source(ids);
          if (!rows) return std::optional<std::vector<c::ClanChannelMember>>{};
          for (const auto& row : *rows) result.push_back(row.member);
          return std::optional{result};
        });
    std::istringstream config(
        enable_clan_channel
            ? "[80]\nMinimumLevel=1\nMaximumLevel=260\nMaximumMercenaries=1\n"
            : "");
    worlds = std::make_shared<ch::ChannelWorlds>(
        ch::ChannelRules::Read(config), parties,
        bf::features::social::RoomInvitations::AllowsPair{},
        bf::features::rooms::RoomMatchSettler{},
        bf::features::rooms::MatchRewardPolicy{}, channel, gate);
    const auto world = worlds->Find(60);
    const std::array<std::u16string, 3> names{u"Anchor", u"Member", u"Guest"};
    for (std::size_t i = 0; i < 3; ++i) {
      stores[i] = std::make_shared<Characters>();
      auto store = stores[i];
      store->record = {10 + i, 100 + i, names[i], 27, 2};
      rows[i] = {{{100 + i, static_cast<std::uint32_t>(10 + i)},
                  1,
                  42,
                  i == 0 ? 101U : 100U,
                  true},
                 u"클랜"};
      players[i] = std::make_unique<Player>(
          [store](auto) { return std::optional{store->record.account_id}; },
          store,
          [this, i](const auto& member, auto observed) {
            auto data = bf::sessions::CreateMenuSnapshot(
                member, {member.account_id, member.character_id, {}}, observed);
            if (fault == static_cast<int>(i))
              data.member.values_at52.resize(4097);
            return data;
          },
          Player::EquipmentChangeHandler{}, Player::ShopCatalogProvider{},
          Player::ShopPurchaseHandler{}, Player::CashPickupHandler{},
          Player::WeaponPresetHandler{}, Player::ItemUseHandler{},
          [this] { return now; }, [] { return 12345U; }, world->rooms,
          world->plazas, nullptr, world->invitations, parties, world->transfers,
          worlds, auth);
      p::ClientToServerMessage0x0001 hello;
      hello.initial_value = 1256;
      players[i]->Process(hello.Encode());
      p::LoginRequest login;
      login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
      players[i]->Process(login.Encode());
      players[i]->Process({521, {}});
      players[i]->Process({106, {}});
      players[i]->EnableChannelHandoff();
    }
    Drain();
    Clear();
  }
  void Clear() {
    for (auto& v : output) v.clear();
  }
  void Collect(std::size_t i) {
    auto packets = players[i]->Poll();
    for (auto& v : packets) output[i].push_back(std::move(v));
  }
  void Drain() {
    for (int round = 0; round < 3; ++round)
      for (std::size_t i = 0; i < 3; ++i)
        if (players[i]) Collect(i);
  }
  auto Target(std::size_t i) {
    return worlds->Find(players[i]->ChannelCategory())
        ->plazas->Capture(100 + i);
  }
  void Join(std::size_t i) {
    now += std::chrono::seconds(4);
    players[0]->Process(
        p::InvitationRequest{stores[i]->record.character_name, 1}.Encode());
    Drain();
    players[i]->Process(p::MoveToInviterRequest{u"Anchor", 0, 0, 1}.Encode());
    Drain();
    Clear();
  }
  void Move(p::TcpPacket packet) {
    Clear();
    output[0] = players[0]->Process(packet);
    Drain();
  }
  void Enter(std::uint64_t id = 42) {
    Move(p::ClanPlazaRequest{id, u"FORGED"}.Encode());
  }
  void Scene(std::size_t i, std::uint32_t type, std::size_t count,
             std::source_location at = std::source_location::current()) {
    auto found =
        std::find_if(output[i].begin(), output[i].end(),
                     [](const auto& v) { return v.message_id == 117; });
    Check(found != output[i].end());
    auto scene = p::GotoRoomNotification::Decode(*found);
    if (scene.descriptor.session_type != type || scene.members.size() != count)
      std::cerr << "Scene assertion at " << at.line() << ": type "
                << scene.descriptor.session_type << " count "
                << scene.members.size() << " expected " << type << "/" << count
                << "\n";
    Check(scene.descriptor.session_type == type &&
          scene.members.size() == count);
    Check(scene.members[scene.value_at728].value_at8 == 10 + i);
    Check(type != 6 || (scene.descriptor.text_at88 == u"클랜" &&
                        scene.descriptor.wide_at80 == UINT64_MAX));
    Check(!Has(output[i], 2029));
  }
  c::ClanAuthentication::Binding Bind(std::size_t i) {
    auto packets = players[i]->Process({25, {}});
    auto found = std::find_if(packets.begin(), packets.end(),
                              [](const auto& v) { return v.message_id == 21; });
    Check(found != packets.end());
    return auth->Consume(p::ClanPassportResponse::Decode(*found).credentials);
  }
  static p::CreateRoomRequest Room() {
    p::CreateRoomRequest request;
    request.settings.text_at8 = u"PrivateRoom";
    request.settings.configuration.short_at4 = 1;
    return request;
  }
};
void Solo() {
  Fixture f;
  auto before = f.Target(0);
  for (auto id : {0ULL, 43ULL, 0x80000000ULL, 0xFFFFFFFFFFFFFFFFULL}) {
    f.Enter(id);
    Check(f.Target(0)->token == before->token && !Has(f.output[0], 117));
  }
  f.Enter();
  f.Scene(0, 6, 1);
  Check(f.Target(0)->clan_id == 42 && f.Target(1)->clan_id == 0);
  Check(f.Target(0)->scene != f.Target(1)->scene);
  auto binding = f.Bind(0);
  Check(binding.Scene() == std::optional{c::ClanGameScene{60, 6}} &&
        !binding.PlazaEpoch());
  f.Move({105, {}});
  f.Scene(0, 1, 1);
  Check(!f.Target(0));
  f.Enter();
  f.Scene(0, 6, 1);
  f.Move({106, {}});
  f.Scene(0, 3, 3);
  Check(f.Target(0)->clan_id == 0);
  Check(binding.Scene() == std::optional{c::ClanGameScene{60, 3}} &&
        binding.PlazaEpoch());
  f.rows[0].member.clan_id = 99;
  f.rows[0].member.membership_status = 100;
  f.Enter(99);
  f.Scene(0, 6, 1);
  Check(f.Target(0)->clan_id == 99);
  f.Move({106, {}});
  Check(!f.players[0]->BeginConnectionHandoff(80));
}
void Group() {
  Fixture f;
  f.Join(1);
  f.Join(2);
  auto before = f.Target(0);
  auto denied =
      f.players[1]->Process(p::ClanPlazaRequest{42, u"클랜"}.Encode());
  Check(!Has(denied, 117) && f.Target(0)->token == before->token);
  f.Enter();
  for (std::size_t i = 0; i < 3; ++i) {
    f.Scene(i, 6, 3);
    Check(f.Target(i)->clan_id == 42);
  }
  Check(f.Target(0)->scene == f.Target(1)->scene &&
        f.Target(1)->scene == f.Target(2)->scene);
  f.Move(Fixture::Room().Encode());
  const auto room = f.worlds->Find(60)->rooms->FindByCharacter(10);
  Check(room.has_value());
  f.Enter();
  for (std::size_t i = 0; i < 3; ++i) {
    f.Scene(i, 6, 3);
    Check(!f.worlds->Find(60)->rooms->FindByCharacter(10 + i));
  }
  Check(!f.worlds->Find(60)->rooms->Find(*room));
  f.Move({105, {}});
  for (std::size_t i = 0; i < 3; ++i) {
    f.Scene(i, 1, 1);
    Check(!f.Target(i));
  }
}
void Revalidation() {
  for (int failure = 0; failure < 4; ++failure) {
    Fixture f;
    f.Join(1);
    f.Join(2);
    const std::array before{f.Target(0)->token, f.Target(1)->token,
                            f.Target(2)->token};
    f.output[0] =
        f.players[0]->Process(p::ClanPlazaRequest{42, u"Ignored"}.Encode());
    if (failure == 0) f.rows[1].member.membership_status = 1;
    if (failure == 1) f.rows[2].member.enabled = false;
    if (failure == 2) f.now += std::chrono::seconds(6);
    if (failure == 3) f.fault = 2;
    f.Drain();
    for (std::size_t i = 0; i < 3; ++i) {
      Check(f.Target(i)->token == before[i]);
      Check(!Has(f.output[i], 117));
    }
    f.rows[1].member.membership_status = 100;
    f.rows[2].member.enabled = true;
    f.fault = -1;
    f.Enter();
    for (std::size_t i = 0; i < 3; ++i) f.Scene(i, 6, 3);
  }
  // Shared commit precedes per-session adoption. Residency must already see it.
  Fixture f;
  f.Join(1);
  f.Join(2);
  f.players[0]->Process(p::ClanPlazaRequest{42, u"Ignored"}.Encode());
  f.Collect(1);
  f.Collect(2);
  Check(f.Target(0)->clan_id == 42);
  f.rows[1].member.membership_status = 1;
  f.auth->InvalidateMembership();
  for (auto& player : f.players) Rejects([&] { player->Poll(); });
}
void Invitations() {
  Fixture f;
  f.Enter();
  f.Join(1);
  Check(f.Target(1)->clan_id == 42 && f.Target(1)->scene == f.Target(0)->scene);
  f.rows[2].member.clan_id = 43;
  const auto before = f.Target(2)->token;
  f.Join(2);
  Check(f.Target(2)->token == before && f.Target(2)->clan_id == 0);
  // A target member becoming ineligible after consent invalidates the whole
  // candidate group, even though the visitor itself belongs to the clan.
  f.rows[2].member.clan_id = 42;
  f.now += std::chrono::seconds(4);
  f.players[0]->Process(p::InvitationRequest{u"Guest", 1}.Encode());
  f.Drain();
  f.rows[1].member.membership_status = 1;
  auto refused = f.players[2]->Process(
      p::MoveToInviterRequest{u"Anchor", 0, 0, 1}.Encode());
  Check(!Has(refused, 117) && f.Target(2)->token == before);
  f.auth->InvalidateMembership();
  Rejects([&] { f.players[0]->Poll(); });
  Rejects([&] { f.players[1]->Poll(); });
  Check(!f.players[2]->Poll().empty() || f.Target(2)->clan_id == 0);
}
void ResidentClock() {
  Fixture f;
  f.Enter();
  f.Drain();
  const auto reads = f.reads;
  f.players[0]->Poll();
  Check(f.reads == reads);
  f.rows[0].member.clan_id = 43;
  f.now += std::chrono::seconds(9);
  f.players[0]->Poll();
  Check(f.reads == reads);
  f.now += std::chrono::seconds(1);
  Rejects([&] { f.players[0]->Poll(); });
}
void ChannelContinuity() {
  {
    Fixture f(false);
    Check(!f.worlds->Find(80));
    // Private entry needs neither category80 nor a previously issued C25.
    f.Enter();
    f.Scene(0, 6, 1);
    auto binding = f.Bind(0);
    const auto token = f.Target(0)->token;
    Check(f.players[0]->BeginConnectionHandoff(60));
    Check(binding.Scene() == std::optional{c::ClanGameScene{60, 0}});
    Check(f.players[0]->CompleteConnectionHandoff().empty());
    Check(f.Target(0)->token == token && f.Target(0)->clan_id == 42);
    Check(binding.Scene() == std::optional{c::ClanGameScene{60, 6}} &&
          !binding.PlazaEpoch());
  }
  {
    Fixture f;
    f.Enter();
    Check(f.players[0]->BeginConnectionHandoff(80));
    auto packets = f.players[0]->CompleteConnectionHandoff();
    Check(packets.size() == 1 &&
          p::GotoRoomNotification::Decode(packets[0]).descriptor.session_type ==
              3);
    Check(f.Target(0)->clan_id == 0 &&
          !f.worlds->Find(60)->plazas->Capture(100));
    f.Enter();
    f.Scene(0, 6, 1);
    Check(f.players[0]->ChannelCategory() == 80 && f.Target(0)->clan_id == 42);
    Check(f.players[0]->BeginConnectionHandoff(60));
    packets = f.players[0]->CompleteConnectionHandoff();
    Check(packets.size() == 1 &&
          p::GotoRoomNotification::Decode(packets[0]).descriptor.session_type ==
              3);
    Check(f.Target(0)->clan_id == 0 &&
          !f.worlds->Find(80)->plazas->Capture(100));
  }
}
}  // namespace
int RunClanPlazaSessionTests() {
  Solo();
  Group();
  Revalidation();
  Invitations();
  ResidentClock();
  ChannelContinuity();
  std::cout << "Clan plaza sessions: " << checks << " checks passed.\n";
  return checks;
}
