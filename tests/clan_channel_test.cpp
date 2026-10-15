#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <source_location>
#include <sstream>

#include "features/channels/channel_worlds.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/scenes/channel_transfer.h"
#include "sessions/clan_session.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace {
namespace bf = bubble_fighter::server;
namespace c = bf::features::clans;
namespace ch = bf::features::channels;
namespace p = bf::protocol;
namespace party = bf::features::parties;
using Character = bf::features::characters::CharacterRecord;
using Auth = c::ClanAuthentication;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Clan channel line " + std::to_string(at.line()));
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
ch::ChannelRules Rules(const std::string& text) {
  std::istringstream input(text);
  return ch::ChannelRules::Read(input);
}
const std::string config =
    "[80]\nMinimumLevel=1\nMaximumLevel=260\nMaximumMercenaries=1\n";
void Authority() {
  auto auth = std::make_shared<Auth>();
  std::vector<c::ClanIdentity> identities;
  std::optional<std::vector<c::ClanChannelMember>> rows;
  int reads = 0, commits = 0;
  c::ClanChannelAccess gate(auth, [&](auto) {
    ++reads;
    return rows;
  });
  struct Case {
    std::vector<std::uint32_t> clans, roles;
    std::uint32_t limit, result;
  };
  const std::vector<Case> cases{
      {{42}, {100}, 1, 0},
      {{42}, {101}, 1, 0},
      {{42}, {102}, 1, 0},
      {{0}, {0}, 1, 3},
      {{42}, {1}, 1, 3},
      {{99}, {100}, 1, 3},
      {{42, 42, 42, 42}, {101, 102, 100, 100}, 0, 0},
      {{42, 0}, {101, 0}, 1, 0},
      {{42, 43}, {101, 100}, 1, 0},
      {{42, 42}, {101, 1}, 1, 0},
      {{42, 99}, {101, 100}, 1, 0},
      {{42, 43, 44}, {101, 100, 100}, 1, 4},
      {{42, 0}, {101, 0}, 0, 4},
      {{42, 43, 0, 99}, {101, 100, 0, 100}, 3, 0},
      {{42, 43, 0, 99, 44}, {101, 100, 0, 100, 1}, 3, 4}};
  for (const auto& test : cases) {
    identities.clear();
    rows.emplace();
    commits = 0;
    for (std::uint32_t i = 0; i < test.clans.size(); ++i) {
      identities.push_back({100 + i, 10 + i});
      rows->push_back(
          {identities.back(), 20, test.clans[i], test.roles[i], true});
    }
    const auto result = gate.Authorize(80, 1, 260, test.limit, identities, [&] {
      ++commits;
      return true;
    });
    Check(result.result == test.result);
    Check(commits == (test.result == 0 ? 1 : 0));
  }
  identities = {{1, 10}};
  const std::vector<c::ClanChannelMember> valid{{{1, 10}, 20, 42, 101, true}};
  const auto deny = [&] {
    commits = 0;
    Check(gate.Authorize(80, 1, 260, 1, identities,
                         [&] {
                           ++commits;
                           return true;
                         })
              .result == 2);
    Check(commits == 0);
  };
  rows.reset();
  deny();
  rows.emplace();
  deny();
  rows = valid;
  rows->front().identity.account_id = 2;
  deny();
  rows = valid;
  rows->front().identity.character_id = 11;
  deny();
  rows = valid;
  rows->front().enabled = false;
  deny();
  for (auto level : {0U, 261U}) {
    rows = valid;
    rows->front().level = level;
    deny();
  }
  rows = valid;
  rows->push_back(rows->front());
  deny();
  rows = valid;
  const auto before = reads;
  for (const auto& bad : std::vector<std::vector<c::ClanIdentity>>{
           {},
           {{0, 10}},
           {{1, 0}},
           {{1, 10}, {1, 11}},
           {{1, 10}, {2, 10}},
           std::vector<c::ClanIdentity>(9, {1, 10})}) {
    Check(gate.Authorize(80, 1, 260, 1, bad).result == 2);
  }
  Check(gate.Authorize(81, 1, 260, 1, identities).result == 2);
  Check(gate.Authorize(80, 0, 260, 1, identities).result == 2);
  Check(gate.Authorize(80, 30, 20, 1, identities).result == 2);
  Check(gate.Authorize(80, 1, 261, 1, identities).result == 2);
  Check(gate.Authorize(80, 1, 260, 4, identities).result == 2);
  Check(gate.Authorize(80, 1, 260, 1, identities, {}, true).result == 2);
  Check(reads == before);
  auto main = auth->BindGame(identities.front());
  auto binding = auth->Consume(auth->Issue(main, u"Anchor"));
  auth->UpdateScene(main, {60, 3});
  const auto epoch = binding.PlazaEpoch();
  Check(gate.Authorize(
                80, 1, 260, 1, identities, [] { return false; }, true)
            .result == 2);
  Check(binding.PlazaEpoch() == epoch && !binding.InClanChannel());
  Rejects([&] {
    gate.Authorize(
        80, 1, 260, 1, identities,
        []() -> bool { throw std::runtime_error("Synthetic scene failure"); },
        true);
  });
  Check(binding.PlazaEpoch() == epoch);
  Check(gate.Authorize(
                80, 1, 260, 1, identities, [] { return true; }, true)
            .result == 0);
  Check(binding.InClanChannel() && !binding.PlazaEpoch());
  auth->UpdateScene(
      main, {60, 3});  // Delayed old-scene adoption cannot restore authority.
  Check(binding.InClanChannel() && !binding.PlazaEpoch());
  auto replacement = auth->Consume(auth->Issue(main, u"Anchor"));
  Check(replacement.InClanChannel() && !binding.InClanChannel());
  rows.reset();
  const auto old_reads = reads;
  Check(gate.Authorize(
                60, 1, 260, 1, identities, [] { return true; }, true)
            .result == 0);
  Check(reads == old_reads && !replacement.InClanChannel());
  auth->UpdateScene(main, {60, 3});
  Check(replacement.PlazaEpoch().has_value());
  rows = valid;
  std::promise<void> entered, release, writer_started;
  auto wait = release.get_future().share();
  auto admission = std::async(std::launch::async, [&] {
    return gate.Authorize(
        80, 1, 260, 1, identities,
        [&] {
          entered.set_value();
          wait.wait();
          return true;
        },
        true);
  });
  entered.get_future().wait();
  auto writer = std::async(std::launch::async, [&] {
    writer_started.set_value();
    auto lock = auth->LockOperations();
    const auto observed = replacement.InClanChannel();
    auth->InvalidateMembership();
    return observed;
  });
  writer_started.get_future().wait();
  const bool blocked = writer.wait_for(std::chrono::milliseconds(30)) ==
                       std::future_status::timeout;
  release.set_value();
  const auto decision = admission.get();
  const bool published = writer.get();
  Check(blocked && published && decision.result == 0);
  Check(gate.Revision() != decision.revision);
  Rejects([&] {
    c::ClanChannelAccess invalid(nullptr,
                                 [&](auto) { return std::optional{valid}; });
  });
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
  std::shared_ptr<Auth> auth = std::make_shared<Auth>();
  std::shared_ptr<party::PartyRegistry> parties =
      std::make_shared<party::PartyRegistry>(8);
  std::shared_ptr<c::ClanChannelAccess> gate;
  std::shared_ptr<ch::ChannelWorlds> worlds;
  std::array<std::shared_ptr<Characters>, 3> stores;
  std::array<std::unique_ptr<Player>, 3> players;
  std::array<c::ClanChannelMember, 3> rows;
  std::array<Auth::Binding, 3> bindings;
  int reads = 0;
  Fixture() {
    gate = std::make_shared<c::ClanChannelAccess>(auth, [this](auto ids) {
      ++reads;
      std::vector<c::ClanChannelMember> result;
      for (const auto& id : ids) {
        const auto found =
            std::find_if(rows.begin(), rows.end(),
                         [&](const auto& row) { return row.identity == id; });
        if (found == rows.end())
          return std::optional<std::vector<c::ClanChannelMember>>{};
        result.push_back(*found);
      }
      return std::optional{result};
    });
    worlds = std::make_shared<ch::ChannelWorlds>(
        Rules(config), parties,
        bf::features::social::RoomInvitations::AllowsPair{},
        bf::features::rooms::RoomMatchSettler{},
        bf::features::rooms::MatchRewardPolicy{}, gate);
    auto world = worlds->Find(60);
    const std::array<std::u16string, 3> names{u"Anchor", u"Member", u"Guest"};
    for (std::size_t i = 0; i < 3; ++i) {
      stores[i] = std::make_shared<Characters>();
      auto store = stores[i];
      store->record = {10 + i, 100 + i, names[i], 27, 2};
      rows[i] = {{100 + i, static_cast<std::uint32_t>(10 + i)},
                 1,
                 42,
                 i == 0 ? 101U : 100U,
                 true};
      players[i] = std::make_unique<Player>(
          [store](auto) { return std::optional{store->record.account_id}; },
          store,
          [](const auto& member, auto observed) {
            return bf::sessions::CreateMenuSnapshot(
                member, {member.account_id, member.character_id, {}}, observed);
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
      p::LoginRequest request;
      request.nexon_passport = u"BFS1." + std::u16string(64, u'0');
      players[i]->Process(request.Encode());
      players[i]->Process({521, {}});
      players[i]->Process({106, {}});
      players[i]->EnableChannelHandoff();
    }
    Drain();
  }
  void Bind(std::size_t i) {
    const auto response = players[i]->Process({25, {}});
    const auto packet =
        std::find_if(response.begin(), response.end(),
                     [](const auto& v) { return v.message_id == 21; });
    Check(packet != response.end());
    bindings[i] =
        auth->Consume(p::ClanPassportResponse::Decode(*packet).credentials);
  }
  void Drain() {
    for (auto& player : players)
      if (player) player->Poll();
  }
  void Join(std::size_t visitor) {
    now += std::chrono::seconds{4};
    players[0]->Process(
        p::InvitationRequest{stores[visitor]->record.character_name, 1}
            .Encode());
    Drain();
    players[visitor]->Process(
        p::MoveToInviterRequest{u"Anchor", 0, 0, 1}.Encode());
    Drain();
  }
  void Solo(std::size_t i, std::uint32_t category) {
    Check(players[i]->BeginConnectionHandoff(category));
    auto result = players[i]->CompleteConnectionHandoff();
    Check(players[i]->ChannelCategory() == category);
    Check(result.size() == 1 && result.front().message_id == 117);
  }
  void Prepare(std::uint32_t category) {
    Check(players[0]->BeginPartyChannelHandoff(category) ==
          std::optional{true});
    for (auto& player : players) player->PollPartyChannelHandoff();
    for (auto& player : players) {
      Check(player->PollPartyChannelHandoff().phase ==
            ch::ChannelMovePhase::kOffer);
      Check(player->BeginConnectionHandoff(category));
    }
    for (auto& player : players)
      Check(player->SignalPartyChannelHandoff(ch::ChannelMoveSignal::kEntered));
    for (auto& player : players)
      Check(
          player->SignalPartyChannelHandoff(ch::ChannelMoveSignal::kSwitched));
  }
  void Commit() {
    for (auto& player : players)
      Check(player->SignalPartyChannelHandoff(
          ch::ChannelMoveSignal::kAcknowledged));
  }
  void Adopt() {
    for (auto& player : players) player->CompleteConnectionHandoff();
    Drain();
  }
};
void Sessions() {
  {
    Fixture f;
    f.rows[0].clan_id = 0;
    f.rows[0].membership_status = 0;
    Check(!f.players[0]->BeginConnectionHandoff(80));
    Check(f.players[0]->ChannelRefusal() == 3 &&
          f.worlds->Find(60)->plazas->Capture(100));
    Check(!f.worlds->Find(80)->plazas->Capture(100));
    f.rows[0].clan_id = 42;
    f.rows[0].membership_status = 101;
    Check(f.players[0]->BeginConnectionHandoff(80));
    f.rows[0].enabled = false;
    Rejects([&] { f.players[0]->CompleteConnectionHandoff(); });
    Check(!f.worlds->Find(80)->plazas->Capture(100));
    Check(f.worlds->Find(60)->plazas->Capture(100).has_value());
    f.players[0]->EndConnectionHandoff();
    f.rows[0].enabled = true;
    f.Solo(
        0,
        80);  // No C25 before admission; a later binding must retain category.
    f.Bind(0);
    Check(f.bindings[0].InClanChannel() && !f.bindings[0].PlazaEpoch());
    f.Solo(0, 60);
    Check(!f.bindings[0].InClanChannel() && f.bindings[0].PlazaEpoch());
  }
  {
    Fixture f;
    f.Join(1);
    f.Join(2);
    f.rows[1].clan_id = 43;
    f.rows[2].clan_id = 44;
    Check(f.players[0]->BeginPartyChannelHandoff(80) == std::optional{false});
    Check(f.players[0]->ChannelRefusal() == 4);
    Check(f.players[0]->BeginPartyChannelHandoff(81) == std::optional{false});
    Check(f.players[0]->ChannelRefusal() == 2);
    for (std::uint64_t i = 100; i < 103; ++i) {
      Check(f.worlds->Find(60)->plazas->Capture(i).has_value());
      Check(!f.worlds->Find(80)->plazas->Capture(i));
    }
    f.rows[1].clan_id = 42;
    for (std::size_t i = 0; i < 3; ++i) f.Bind(i);
    f.Prepare(80);
    f.Commit();
    for (const auto& b : f.bindings)
      Check(b.InClanChannel() && !b.PlazaEpoch());
    // No session has adopted80 yet, but all auxiliary connections are guarded.
    for (const auto& player : f.players) Check(player->ChannelCategory() == 60);
    f.Adopt();
    const auto scene = f.worlds->Find(80)->plazas->Capture(100)->scene;
    for (std::uint64_t i = 100; i < 103; ++i)
      Check(f.worlds->Find(80)->plazas->Capture(i)->scene == scene);
    f.Prepare(60);
    f.Commit();
    f.Adopt();
    for (const auto& b : f.bindings)
      Check(!b.InClanChannel() && b.PlazaEpoch());
  }
  {
    Fixture f;
    f.Join(1);
    f.Join(2);
    f.Prepare(80);
    f.rows[1].clan_id = 43;
    f.rows[2].clan_id = 44;
    f.Commit();
    for (auto& player : f.players)
      Check(player->PollPartyChannelHandoff().phase ==
            ch::ChannelMovePhase::kClosed);
    for (std::uint64_t i = 100; i < 103; ++i)
      Check(!f.worlds->Find(80)->plazas->Capture(i));
  }
  for (bool external : {false, true}) {
    Fixture f;
    f.Join(1);
    f.Join(2);
    f.Prepare(80);
    f.Commit();
    f.Adopt();
    const auto reads = f.reads;
    f.Drain();
    Check(f.reads == reads);
    f.rows[0].clan_id = 0;
    f.rows[0].membership_status = 0;
    if (external)
      f.now += std::chrono::seconds{10};
    else {
      auto lock = f.auth->LockOperations();
      f.auth->InvalidateMembership();
    }
    // Losing the leader could make the two remaining same-clan members valid;
    // the atomic revoked marks still close every member of the original group.
    for (auto& player : f.players) Rejects([&] { player->Poll(); });
    for (std::uint64_t i = 100; i < 103; ++i)
      Check(!f.worlds->Find(80)->plazas->Capture(i));
  }
  {
    Fixture f;
    for (std::size_t i = 0; i < 3; ++i) f.Solo(i, 80);
    f.rows[1].clan_id = 43;
    f.rows[2].clan_id = 44;
    f.Join(1);
    const auto target = f.parties->Capture(u"Anchor");
    Check(f.parties->Capture(u"Member")->party_id == target->party_id);
    f.Join(2);  // Whole destination would exceed its one-mercenary limit.
    Check(f.parties->Capture(u"Guest")->party_id != target->party_id);
    Check(f.worlds->Find(80)->plazas->Capture(102).has_value());
    // Existing consent preserves party epoch; identity-list cache detects join.
    Check(f.players[0]->ChannelCategory() == 80);
  }
}
void LeaveGuard() {
  auto auth = std::make_shared<Auth>();
  auto main = auth->BindGame({1, 10});
  c::ClanSnapshot snapshot;
  snapshot.profile.clan_id = 42;
  snapshot.profile.membership_status = 100;
  int mutations = 0, training = 0;
  bf::sessions::ClanSession session(
      auth, [&](auto) { return std::optional{snapshot}; }, Auth::Clock::now, {},
      {}, {},
      [&](auto, const auto&, const auto& authorized) {
        Check(authorized());
        ++mutations;
        return c::ClanMutationResult{};
      },
      nullptr, {},
      [&](auto, auto, const auto& authorized) {
        Check(authorized());
        ++training;
        return c::TrainingClanResult{false};
      });
  session.Process(p::ClanLoginRequest{auth->Issue(main, u"Anchor")}.Encode());
  {
    auto lock = auth->LockOperations();
    auth->PublishChannelCategory(std::array{c::ClanIdentity{1, 10}}, 80);
  }
  for (const auto& packet : std::vector<p::ClanPacket>{
           p::ClanLeaveRequest{}.Encode(),
           p::TrainingClanExitRequest{0xffffffec}.Encode()}) {
    const auto response = session.Process(packet);
    Check(!response.close_connection && response.responses.size() == 1 &&
          response.responses[0].message_id == 1016);
    Check(p::ClanProfileResponse::Decode(response.responses[0]).clan_id == 42);
  }
  Check(mutations == 0 && training == 0);
  session.Process(p::TrainingClanExitRequest{0xffffffc4}.Encode());
  Check(training ==
        1);  // Graduation must still be reconciled by trusted DB policy.
  {
    auto lock = auth->LockOperations();
    auth->PublishChannelCategory(std::array{c::ClanIdentity{1, 10}}, 60);
  }
  session.Process(p::ClanLeaveRequest{}.Encode());
  Check(mutations == 1);
}
void RevokedReservation() {
  auto registry = std::make_shared<party::PartyRegistry>(8);
  Character first{10, 1, u"First", 27, 2}, second{20, 2, u"Second", 27, 2};
  auto a = registry->Enter(first, [](auto) {});
  auto b = registry->Enter(second, [](auto) {});
  Check(b.JoinApproved(*registry->Capture(u"First")) ==
        party::PartyJoinStatus::kJoined);
  auto reservation = registry->ReserveScene(a);
  Check(reservation.has_value());
  Check(
      !a.CheckAccess([](auto, auto, auto members) { return members.empty(); }));
  int commits = 0;
  Check(!reservation->Commit([&] {
    ++commits;
    return true;
  }));
  Check(commits == 0);
  reservation.reset();
  a.Update(first, false);  // Updating display data must retain revocation.
  Check(!a.CheckAccess([](auto, auto, auto) { return true; }));
  a.Release();
  Check(!b.CheckAccess([](auto, auto, auto) { return true; }));
  Check(!registry->ReserveScene(b, true));
}
}  // namespace
int RunClanChannelTests(const std::filesystem::path& directory) {
  std::ifstream input(directory / "clan_channel_packets.txt");
  Check(input.good());
  std::string label, hex;
  std::uint16_t id = 0;
  int fixtures = 0;
  while (input >> label >> id >> hex) {
    p::TcpPacket packet{id, {}};
    for (std::size_t i = 0; i < hex.size(); i += 2)
      packet.payload.push_back(
          static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    if (id == 53)
      Check(p::MoveChannelRequest::Decode(packet).channel_category == 80);
    else {
      const auto decoded = p::MoveChannelResponse::Decode(packet);
      Check(decoded.channel_category == 80 &&
            decoded.Encode().payload == packet.payload);
      const auto expected = label == "requires_clan"          ? 3U
                            : label == "too_many_mercenaries" ? 4U
                            : label == "unavailable"          ? 2U
                                                              : 0U;
      Check(decoded.result == expected);
    }
    ++fixtures;
  }
  Check(fixtures == 5);
  auto parties = std::make_shared<party::PartyRegistry>(8);
  Check(!ch::ChannelRules{}.Categories().contains(80));
  Rejects([&] { ch::ChannelWorlds unavailable(Rules(config), parties); });
  for (const auto& text : std::vector<std::string>{
           "[80]\nMinimumLevel=1\nMaximumLevel=260\n",
           config + "MaximumMercenaries=1\n",
           "[80]\nMinimumLevel=1\nMaximumLevel=260\nMaximumMercenaries=4\n",
           "[20]\nMinimumLevel=1\nMaximumLevel=260\nMaximumMercenaries=1\n"})
    Rejects([&] { Rules(text); });
  Authority();
  Sessions();
  LeaveGuard();
  RevokedReservation();
  std::cout << "Clan channel: " << checks << " checks passed.\n";
  return checks;
}
