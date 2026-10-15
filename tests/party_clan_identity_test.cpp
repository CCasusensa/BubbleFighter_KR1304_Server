#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
#include <thread>

#include "features/rooms/room_packet_queue.h"
#include "protocol/clans/clan_state_report.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace {
namespace bf = bubble_fighter::server;
namespace c = bf::features::clans;
namespace p = bf::protocol;
namespace party = bf::features::parties;
using Character = bf::features::characters::CharacterRecord;
using Auth = c::ClanAuthentication;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Party clan identity line " +
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
void Clan(const p::UpdatePartyNotification& roster,
          std::initializer_list<std::uint32_t> clans) {
  Check(roster.state == 2 && roster.members.size() == clans.size());
  std::size_t i = 0;
  for (auto id : clans) {
    Check(roster.members[i++].values_at32 ==
          std::array<std::uint32_t, 4>{0xffffffff, 0xffffffff, 0, id});
  }
}
void Registry() {
  auto registry = std::make_shared<party::PartyRegistry>(4);
  auto q1 = std::make_shared<bf::features::rooms::RoomPacketQueue>();
  auto q2 = std::make_shared<bf::features::rooms::RoomPacketQueue>();
  auto q3 = std::make_shared<bf::features::rooms::RoomPacketQueue>();
  Character first{10, 1, u"First", 27, 2}, second{20, 2, u"Second", 27, 2},
      third{30, 3, u"Third", 27, 2};
  auto a = registry->Enter(first, q1->Sink(), 42);
  auto b = registry->Enter(second, q2->Sink(), 99);
  auto d = registry->Enter(third, q3->Sink());
  Clan(a.Snapshot(), {42});
  Clan(b.Snapshot(), {99});
  const auto target = *registry->Capture(u"First");
  Check(b.JoinApproved(target) == party::PartyJoinStatus::kJoined);
  Clan(a.Snapshot(), {42, 99});
  q1->Clear();
  q2->Clear();
  p::PartySupplementaryRecord equipment;
  equipment.key = 20;
  equipment.value = 300027;
  equipment.groups[0] = {201005, 207001};
  b.Update(second, false, equipment);
  q1->Clear();
  q2->Clear();
  auto reservation = registry->ReserveScene(a);
  Check(reservation.has_value());
  b.Update(second, false, equipment, 42);
  Check(!reservation->Commit([] { return true; }));
  reservation.reset();
  Check(d.JoinApproved(target) == party::PartyJoinStatus::kStale);
  Clan(a.Snapshot(), {42, 42});
  const auto first_update = q1->Drain(), second_update = q2->Drain();
  Check(first_update.size() == 1 && second_update.size() == 1);
  Check(first_update[0].message_id == 201 &&
        first_update[0].payload == second_update[0].payload);
  const auto roster = p::UpdatePartyNotification::Decode(first_update[0]);
  Clan(roster, {42, 42});
  Check(roster.members[0].identity_value == 10 &&
        roster.members[1].character_name == u"Second" &&
        roster.supplementary_records == std::vector{equipment});
  const auto epoch = registry->Capture(u"First")->epoch;
  b.Update(second, false, equipment, 42);
  b.Update(second, false, equipment);  // Scene call preserves the cache.
  Check(q1->Drain().empty() && q2->Drain().empty());
  Check(registry->Capture(u"First")->epoch == epoch);
  auto bad = second;
  bad.account_id = 77;
  Rejects([&] { b.Update(bad, false, equipment, 123); });
  Clan(b.Snapshot(), {42, 42});
  b.Update(second, false, equipment, 0);
  Clan(b.Snapshot(), {42, 0});
  Check(q1->Drain().size() == 1 && q2->Drain().size() == 1);
  a.ChangeLeader(u"Second");
  Clan(a.Snapshot(), {0, 42});
  Check(a.Leave());
  Clan(a.Snapshot(), {42});
  Clan(b.Snapshot(), {0});
  b.Release();
  b = registry->Enter(second, q2->Sink(), 43);
  Clan(b.Snapshot(), {43});
}
struct Characters final : bf::features::characters::CharacterStore {
  Character record{10, 1, u"First", 27, 2};
  std::optional<Character> FindByAccount(std::uint64_t id) override {
    return id == 1 ? std::optional{record} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(Character&) override {
    throw std::logic_error("Unexpected create");
  }
};
void Main() {
  using Player = bf::sessions::PlayerSession;
  using Practice = bf::features::practice::PracticeSession;
  auto auth = std::make_shared<Auth>();
  auto characters = std::make_shared<Characters>();
  auto parties = std::make_shared<party::PartyRegistry>(8);
  Practice::Time now{};
  std::optional<c::ClanSnapshot> snapshot{c::ClanSnapshot{}};
  snapshot->profile.clan_id = 42;
  snapshot->profile.membership_status = 101;
  int reads = 0;
  bool fail = false;
  const auto source = [&](const c::ClanIdentity& id) {
    ++reads;
    Check(id == c::ClanIdentity{1, 10});
    if (fail) throw std::runtime_error("Synthetic clan read failure");
    return snapshot;
  };
  const auto make = [&] {
    return std::make_unique<Player>(
        [](auto) { return std::optional<std::uint64_t>{1}; }, characters,
        [](const auto& character, auto observed) {
          return bf::sessions::CreateMenuSnapshot(
              character, {character.account_id, character.character_id, {}},
              observed);
        },
        Player::EquipmentChangeHandler{}, Player::ShopCatalogProvider{},
        Player::ShopPurchaseHandler{}, Player::CashPickupHandler{},
        Player::WeaponPresetHandler{}, Player::ItemUseHandler{},
        [&] { return now; }, [] { return 12345U; }, nullptr, nullptr, nullptr,
        nullptr, parties, nullptr, nullptr, auth, c::ClanConfirmationHandler{},
        c::TrainingClanReconciler{}, source);
  };
  const auto login = [&](Player& player, std::uint32_t clan) {
    p::ClientToServerMessage0x0001 initial;
    initial.initial_value = 1256;
    player.Process(initial.Encode());
    p::LoginRequest request;
    request.nexon_passport = u"BFS1." + std::u16string(64, u'0');
    const auto packets = player.Process(request.Encode());
    Check(packets.size() == 2 && packets[0].message_id == 3);
    Clan(p::UpdatePartyNotification::Decode(packets[1]), {clan});
    Check(!auth->Find(10).Identity());  // No early C25 binding.
  };
  const auto invalidate = [&] {
    auto operation = auth->LockOperations();
    auth->InvalidateMembership();
  };
  auto player = make();
  login(*player, 42);
  Check(reads == 1);
  player->Process({521, {}});
  Check(player->Poll().empty() && reads == 1);
  auto credentials =
      p::ClanPassportResponse::Decode(player->Process({25, {}}).at(0))
          .credentials;
  auto binding = auth->Consume(credentials);
  Check(binding.Identity().has_value());
  // Native auxiliary reports cannot invalidate or overwrite the display cache.
  const auto revision = auth->MembershipRevision();
  player->Process(p::ClanStateReport{999999, u"Forged", 101}.Encode());
  player->Process(p::ClanAppearanceReport{999999, 255, 999999}.Encode());
  Check(player->Poll().empty() && reads == 1 &&
        auth->MembershipRevision() == revision);
  binding = {};
  std::uint32_t last_clan = 42;
  for (const auto role :
       {0U, 1U, 100U, 101U, 102U, 0xffffffecU, 0xffffffc4U, 0xffffffd8U}) {
    const auto before = reads;
    snapshot->profile.membership_status = role;
    const auto previous = snapshot->profile.clan_id;
    snapshot->profile.clan_id = previous == 42 ? 43 : 42;
    invalidate();
    const auto packets = player->Poll();
    Check(reads == before + 1);
    const bool accepted = role >= 100 && role <= 102;
    const auto expected_clan = accepted ? snapshot->profile.clan_id : 0;
    Check(packets.size() == (expected_clan == last_clan ? 0U : 1U));
    if (!packets.empty()) {
      Check(packets.size() == 1);
      Clan(p::UpdatePartyNotification::Decode(packets[0]),
           {accepted ? snapshot->profile.clan_id : 0});
    }
    last_clan = expected_clan;
    Check(player->Poll().empty() && reads == before + 1);
  }
  snapshot->profile.membership_status = 100;
  snapshot->profile.clan_id = 99;
  invalidate();
  Clan(p::UpdatePartyNotification::Decode(player->Poll().at(0)), {99});
  snapshot.reset();
  now += std::chrono::seconds{9};
  Check(player->Poll().empty());
  const auto before = reads;
  now += std::chrono::seconds{1};
  Clan(p::UpdatePartyNotification::Decode(player->Poll().at(0)), {0});
  Check(reads == before + 1);
  player.reset();
  snapshot.emplace();
  snapshot->profile.clan_id = 77;
  snapshot->profile.membership_status = 102;
  player = make();
  login(*player, 77);
  player->Process({521, {}});
  fail = true;
  invalidate();
  Rejects([&] { player->Poll(); });
  Check(player->State() == bf::sessions::PlayerSessionState::kRejected &&
        !parties->Capture(u"First"));
}
}  // namespace
int RunPartyClanIdentityTests(const std::filesystem::path& directory) {
  checks = 0;
  std::ifstream input(directory / "party_clan_identity_packets.txt");
  Check(input.good());
  std::string label, hex;
  int outsiders = 0, fixtures = 0;
  while (input >> label >> outsiders >> hex) {
    p::TcpPacket packet{201, {}};
    for (std::size_t i = 0; i < hex.size(); i += 2)
      packet.payload.push_back(
          static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    const auto roster = p::UpdatePartyNotification::Decode(packet);
    Check(roster.Encode().payload == packet.payload);
    int count = 0;
    for (const auto& member : roster.members)
      if (!member.values_at32[3] ||
          member.values_at32[3] != roster.members.front().values_at32[3])
        ++count;
    Check(count == outsiders);
    ++fixtures;
  }
  Check(fixtures == 12);
  Registry();
  Main();
  auto auth = std::make_shared<Auth>();
  std::array<std::thread, 4> writers;
  for (auto& writer : writers)
    writer = std::thread([&] {
      for (int i = 0; i < 1000; ++i) {
        auto lock = auth->LockOperations();
        auth->InvalidateMembership();
      }
    });
  for (auto& writer : writers) writer.join();
  Check(auth->MembershipRevision() == 4000);
  std::cout << "Party clan identity: " << checks << " checks passed.\n";
  return checks;
}
