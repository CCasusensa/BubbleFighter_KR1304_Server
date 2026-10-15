#include <algorithm>
#include <array>
#include <barrier>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "features/rooms/room_actions.h"
#include "features/rooms/room_map_catalog.h"
#include "protocol/battle/game_loading_complete_request.h"
#include "protocol/battle/game_movement_notification.h"
#include "protocol/battle/game_start_notification.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/rooms/room_host_changed_notification.h"
#include "protocol/rooms/room_ready_request.h"
#include "protocol/rooms/start_game_request.h"
#include "protocol/scenes/channel_transfer.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bf = bubble_fighter::server;
namespace proto = bf::protocol;
namespace party = bf::features::parties;
namespace rooms = bf::features::rooms;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Party scene line " + std::to_string(at.line()));
}
void Sequence(const std::vector<proto::TcpPacket>& packets,
              std::initializer_list<std::uint16_t> ids,
              std::source_location at = std::source_location::current()) {
  const bool match = packets.size() == ids.size() &&
                     std::equal(packets.begin(), packets.end(), ids.begin(),
                                [](const auto& packet, auto id) {
                                  return packet.message_id == id;
                                });
  if (!match) {
    std::cerr << "Party scene sequence at " << at.line() << ":";
    for (const auto& p : packets) std::cerr << " " << p.message_id;
    std::cerr << "\n";
  }
  Check(match, at);
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
    throw std::logic_error("Unexpected create");
  }
};
struct Fixture {
  using Session = bf::sessions::PlayerSession;
  using Time = bf::features::practice::PracticeSession::Time;
  Time now{};
  std::shared_ptr<rooms::RoomRegistry> room_registry =
      rooms::RoomRegistry::Create(
          8, [this] { return now; }, [] { return 12345U; });
  std::shared_ptr<bf::features::lobby::PlazaRegistry> plazas =
      std::make_shared<bf::features::lobby::PlazaRegistry>();
  std::shared_ptr<party::PartyRegistry> parties =
      std::make_shared<party::PartyRegistry>(8);
  std::shared_ptr<bf::features::social::RoomInvitations> invitations =
      std::make_shared<bf::features::social::RoomInvitations>(
          room_registry, bf::features::social::RoomInvitations::AllowsPair{},
          parties, plazas);
  std::shared_ptr<party::PartySceneTransfers> transfers =
      std::make_shared<party::PartySceneTransfers>(parties, room_registry,
                                                   plazas);
  std::array<std::shared_ptr<Characters>, 3> stores;
  std::array<std::unique_ptr<Session>, 3> players;
  std::array<int, 3> faults{}, reads{};
  std::array<std::vector<proto::TcpPacket>, 3> output;
  Fixture() {
    const std::array<std::u16string, 3> names{u"Alice", u"Bob", u"Carol"};
    for (std::size_t i = 0; i < 3; ++i) {
      stores[i] = std::make_shared<Characters>();
      stores[i]->record = {100 + i, 200 + i, names[i], 27, 2};
      const auto c = stores[i]->record;
      players[i] = std::make_unique<Session>(
          [c](std::string_view) { return std::optional{c.account_id}; },
          stores[i],
          [this, i](const auto& character, auto observed) {
            ++reads[i];
            auto data = bf::sessions::CreateMenuSnapshot(
                character, {character.account_id, character.character_id, {}},
                observed);
            if (faults[i] == 1) data.member.values_at52.resize(4097);
            if (faults[i] == 2) ++data.equipment.key;
            return data;
          },
          Session::EquipmentChangeHandler{}, Session::ShopCatalogProvider{},
          Session::ShopPurchaseHandler{}, Session::CashPickupHandler{},
          Session::WeaponPresetHandler{}, Session::ItemUseHandler{},
          [this] { return now; }, [] { return 12345U; }, room_registry, plazas,
          nullptr, invitations, parties, transfers);
      proto::ClientToServerMessage0x0001 hello;
      hello.initial_value = 1256;
      players[i]->Process(hello.Encode());
      proto::LoginRequest login;
      login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
      Sequence(players[i]->Process(login.Encode()), {3, 201});
      Sequence(players[i]->Process({521, {}}), {105, 117, 806, 837, 802, 201});
    }
    for (std::size_t i = 1; i < 3; ++i) {
      now += std::chrono::seconds(4);
      Sequence(
          players[0]->Process(proto::InvitationRequest{names[i], 1}.Encode()),
          {120});
      Sequence(players[i]->Poll(), {121});
      Sequence(players[i]->Process(
                   proto::MoveToInviterRequest{u"Alice", 0, 0, 1}.Encode()),
               {117, 201, 206});
      for (std::size_t j = 0; j < i; ++j)
        Sequence(players[j]->Poll(), {201, 206});
    }
  }
  void Clear() {
    for (auto& packets : output) packets.clear();
  }
  void Collect(std::size_t i) {
    for (auto& packet : players[i]->Poll())
      output[i].push_back(std::move(packet));
  }
  void Drain() {
    for (std::size_t round = 0; round < 3; ++round)
      for (std::size_t i = 0; i < 3; ++i)
        if (players[i]) Collect(i);
  }
  void Move(const proto::TcpPacket& request, bool parallel = false) {
    Clear();
    output[0] = players[0]->Process(request);
    if (parallel) {
      std::barrier gate(3);
      std::array<std::exception_ptr, 2> failures;
      std::thread a([&] {
        gate.arrive_and_wait();
        try {
          Collect(1);
        } catch (...) {
          failures[0] = std::current_exception();
        }
      });
      std::thread b([&] {
        gate.arrive_and_wait();
        try {
          Collect(2);
        } catch (...) {
          failures[1] = std::current_exception();
        }
      });
      gate.arrive_and_wait();
      a.join();
      b.join();
      Check(!failures[0] && !failures[1]);
    }
    Drain();
  }
  void Snapshot(std::size_t i, std::uint32_t type, std::size_t count) {
    const auto found =
        std::find_if(output[i].begin(), output[i].end(),
                     [](const auto& p) { return p.message_id == 117; });
    Check(found != output[i].end());
    const auto scene = proto::GotoRoomNotification::Decode(*found);
    Check(scene.descriptor.session_type == type &&
          scene.members.size() == count);
    Check(scene.value_at728 < count);
    Check(scene.members[scene.value_at728].value_at8 ==
          stores[i]->record.character_id);
    Check(scene.records_at588.size() == count);
    for (const auto& member : scene.members) {
      Check(member.booleans_at0[0] == 1);
      Check(std::count_if(
                scene.records_at588.begin(), scene.records_at588.end(),
                [&](const auto& e) { return e.key == member.value_at8; }) == 1);
    }
  }
  static proto::CreateRoomRequest Create() {
    proto::CreateRoomRequest request;
    request.settings.text_at8 = u"PartyRoom";
    request.settings.configuration.short_at4 = 1;
    return request;
  }
  static proto::StartGameRequest Start() {
    const auto& m = rooms::RoomMapCatalog::Maps().front();
    return {0,
            std::u16string(m.directory),
            std::u16string(m.name),
            std::u16string(m.key),
            m.difficulty,
            m.rules,
            0};
  }
};

void ChannelRefusals(Fixture& f) {
  for (std::size_t i = 0; i < 3; ++i) {
    const auto state = f.players[i]->State();
    const auto character = f.players[i]->Character();
    const auto plaza = f.plazas->Capture(200 + i);
    const auto room_count = f.room_registry->Count();
    auto packets =
        f.players[i]->Process(proto::MoveChannelRequest{70}.Encode());
    Sequence(packets, {8});
    Check(proto::MoveChannelResponse::Decode(packets[0]) ==
          proto::MoveChannelResponse{2, {}, 0, 70});
    Check(f.players[i]->State() == state &&
          f.players[i]->Character()->character_id == character->character_id);
    const auto after = f.plazas->Capture(200 + i);
    Check(plaza.has_value() == after.has_value());
    if (plaza) Check(plaza->token == after->token);
    Check(f.room_registry->Count() == room_count);
    for (auto& player : f.players) Sequence(player->Poll(), {});
  }
}
void WholePartyGame(bool channel_requests = false) {
  Fixture f;
  if (channel_requests) ChannelRefusals(f);
  Sequence(f.players[1]->Process({106, {}}), {509});
  Check(!f.plazas->Capture(201));
  f.Move({106, {}}, true);
  for (std::size_t i = 0; i < 3; ++i) {
    Sequence(f.output[i], {117});
    f.Snapshot(i, 3, 3);
  }
  const auto original = f.plazas->Capture(200);
  if (channel_requests) ChannelRefusals(f);
  f.Move({106, {}}, true);
  Check(f.plazas->Capture(200)->token != original->token);
  for (std::size_t i = 0; i < 3; ++i) Sequence(f.output[i], {117});
  f.Move({105, {}});
  for (std::size_t i = 0; i < 3; ++i) {
    Sequence(f.output[i], {105, 117, 806, 837, 802});
    f.Snapshot(i, 1, 1);
    Check(!f.plazas->Capture(200 + i));
  }
  f.Move({106, {}});
  f.Move(Fixture::Create().Encode(), true);
  Check(f.room_registry->Count() == 1);
  for (std::size_t i = 0; i < 3; ++i) {
    if (i == 0)
      Sequence(f.output[i], {102, 117});
    else
      Sequence(f.output[i], {117});
    f.Snapshot(i, 2, 3);
    Check(f.players[i]->State() == bf::sessions::PlayerSessionState::kInRoom);
    Check(!f.plazas->Capture(200 + i));
  }
  for (std::size_t i = 1; i < 3; ++i) {
    Sequence(f.players[i]->Process(proto::RoomReadyRequest{1, 0, 0}.Encode()),
             {503});
    for (std::size_t j = 0; j < 3; ++j)
      if (j != i) Sequence(f.players[j]->Poll(), {503});
  }
  if (channel_requests) ChannelRefusals(f);
  Sequence(f.players[0]->Process(Fixture::Start().Encode()), {585, 510});
  for (std::size_t i = 1; i < 3; ++i)
    Sequence(f.players[i]->Poll(), {585, 510});
  if (channel_requests) ChannelRefusals(f);
  for (std::size_t i = 0; i < 2; ++i)
    Sequence(
        f.players[i]->Process(proto::GameLoadingCompleteRequest{}.Encode()),
        {606});
  Sequence(f.players[2]->Process(proto::GameLoadingCompleteRequest{}.Encode()),
           {606, 607});
  Sequence(f.players[0]->Poll(), {607});
  Sequence(f.players[1]->Poll(), {607});
  if (channel_requests) ChannelRefusals(f);
  f.now += std::chrono::seconds(4);
  for (std::size_t i = 0; i < 3; ++i) {
    auto packets = f.players[i]->Poll();
    Sequence(packets, {608});
    Check(proto::GameStartNotification::Decode(packets[0]).random_seed ==
          12345);
  }
  if (channel_requests) ChannelRefusals(f);
  const auto move =
      proto::GameMovementNotification{1200000, {1, 2, 3}}.Encode();
  Sequence(f.players[2]->Process(move), {});
  for (std::size_t i = 0; i < 2; ++i) {
    const auto packets = f.players[i]->Poll();
    Sequence(packets, {0x8075});
    Check(packets[0].payload == move.payload);
  }
  Sequence(f.players[0]->Process({624, {1, 0, 0, 0, 0, 0, 0, 0}}), {620, 117});
  for (std::size_t i = 1; i < 3; ++i)
    Sequence(f.players[i]->Poll(), {620, 117});
  if (channel_requests) ChannelRefusals(f);
}

void CancellationAndRecovery() {
  Fixture f;
  f.Move({106, {}});
  std::array<std::uint64_t, 3> tokens;
  for (std::size_t i = 0; i < 3; ++i)
    tokens[i] = f.plazas->Capture(200 + i)->token;
  Sequence(f.players[0]->Process(Fixture::Create().Encode()), {});
  Sequence(f.players[1]->Poll(), {});
  f.now += std::chrono::seconds(5);
  f.Clear();
  f.Drain();
  Sequence(f.output[0], {102});
  Sequence(f.output[1], {});
  Sequence(f.output[2], {});
  Check(f.room_registry->Count() == 0);
  for (std::size_t i = 0; i < 3; ++i)
    Check(f.plazas->Capture(200 + i)->token == tokens[i]);
  // A new client action cancels only the uncommitted operation, restores the
  // lease, then executes against the original scene on that owner's thread.
  Sequence(f.players[0]->Process(Fixture::Create().Encode()), {});
  Sequence(f.players[1]->Process({803, {}}), {802, 837, 806});
  f.Clear();
  f.Drain();
  Sequence(f.output[0], {102});
  Check(f.room_registry->Count() == 0);
  for (std::size_t i = 0; i < 3; ++i)
    Check(f.plazas->Capture(200 + i)->token == tokens[i]);
  // Failure encoding one member must not disconnect the unrelated last
  // participant or publish a partial destination.
  f.faults[0] = 1;
  f.Move(Fixture::Create().Encode());
  Sequence(f.output[0], {102});
  Check(f.room_registry->Count() == 0);
  for (std::size_t i = 0; i < 3; ++i) {
    Check(f.players[i]->State() ==
          bf::sessions::PlayerSessionState::kMenuInitializationSent);
    Check(f.plazas->Capture(200 + i)->token == tokens[i]);
  }
  f.faults[0] = 0;
  f.Move(Fixture::Create().Encode());
  Check(f.room_registry->Count() == 1);
}

void DisconnectAndMalformedActor() {
  Fixture f;
  f.Move({106, {}});
  Sequence(f.players[0]->Process(Fixture::Create().Encode()), {});
  Sequence(f.players[1]->Poll(), {});
  f.players[2].reset();
  f.Clear();
  f.Drain();
  Check(f.room_registry->Count() == 0);
  Check(f.players[0]->State() ==
        bf::sessions::PlayerSessionState::kMenuInitializationSent);
  Check(f.plazas->Capture(200) && f.plazas->Capture(201) &&
        !f.plazas->Capture(202));
  Sequence(f.players[0]->Process(Fixture::Create().Encode()), {});
  f.Clear();
  f.Drain();
  Check(f.room_registry->Count() == 1);

  Fixture malformed;
  malformed.Move({106, {}});
  auto request = Fixture::Create();
  request.settings.short_at0 = 99;
  bool rejected = false;
  try {
    malformed.players[0]->Process(request.Encode());
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  Check(rejected);
  Check(malformed.players[0]->State() ==
        bf::sessions::PlayerSessionState::kRejected);
  for (std::size_t i = 1; i < 3; ++i) {
    malformed.players[i]->Poll();
    Check(malformed.players[i]->State() ==
          bf::sessions::PlayerSessionState::kMenuInitializationSent);
  }
  Check(malformed.room_registry->Count() == 0);
}

void JoiningAndPolicyRefusal() {
  Fixture f;
  f.Move({106, {}});
  bf::features::characters::CharacterRecord owner{500, 600, u"Outside", 27, 2};
  auto data = bf::sessions::CreateMenuSnapshot(owner, {600, 500, {}}, 0);
  const auto queue = std::make_shared<rooms::RoomPacketQueue>();
  auto request = Fixture::Create();
  request.password = u"secret";
  request.settings.boolean_at38 = 1;
  auto created = rooms::RoomCreation::Create(
      *f.room_registry, {{1}, {0}, 20, 6}, request,
      {600, owner, data.member, data.equipment, queue->Sink()});
  Check(static_cast<bool>(created.room));
  queue->Drain();
  f.Move(proto::JoinRoomRequest{created.room.RoomId(), u"wrong"}.Encode());
  Sequence(f.output[0], {103});
  Check(f.room_registry->Find(created.room.RoomId())->occupants.size() == 1);
  for (std::size_t i = 0; i < 3; ++i)
    Check(f.plazas->Capture(200 + i).has_value());
  f.Move(proto::JoinRoomRequest{created.room.RoomId(), u"secret"}.Encode(),
         true);
  for (std::size_t i = 0; i < 3; ++i) {
    if (i == 0)
      Sequence(f.output[i], {103, 117});
    else
      Sequence(f.output[i], {117});
    f.Snapshot(i, 2, 4);
  }
  Sequence(queue->Drain(), {503, 503, 503});
  Fixture unsupported;
  unsupported.Move({106, {}});
  auto other = Fixture::Create();
  other.settings.configuration.short_at4 = 40;
  unsupported.Move(other.Encode());
  Sequence(unsupported.output[0], {102});
  Check(unsupported.room_registry->Count() == 0);
  for (std::size_t i = 0; i < 3; ++i)
    Check(unsupported.plazas->Capture(200 + i).has_value());
}

void ReservationAuthority() {
  auto registry = std::make_shared<party::PartyRegistry>(8);
  auto foreign = std::make_shared<party::PartyRegistry>(8);
  auto sink = [](auto) {};
  bf::features::characters::CharacterRecord a{10, 20, u"Leader", 27, 2};
  bf::features::characters::CharacterRecord b{11, 21, u"Follower", 27, 2};
  bf::features::characters::CharacterRecord c{12, 22, u"Visitor", 27, 2};
  auto leader = registry->Enter(a, sink);
  auto member = registry->Enter(b, sink);
  auto visitor = registry->Enter(c, sink);
  auto outsider = foreign->Enter(a, sink);
  Check(member.JoinApproved(*registry->Capture(a.character_name)) ==
        party::PartyJoinStatus::kJoined);
  Check(!registry->ReserveScene(member));
  Check(!registry->ReserveScene(outsider));
  auto claim = registry->ReserveScene(leader);
  Check(claim.has_value() && claim->Members().size() == 2);
  Check(claim->Members()[0].character.character_id == 10);
  Check(!registry->ReserveScene(leader) && !member.Leave());
  Check(visitor.JoinApproved(*registry->Capture(a.character_name)) ==
        party::PartyJoinStatus::kBusy);
  int commits = 0;
  member.Update(b, true);
  Check(!claim->Commit([&] {
    ++commits;
    return true;
  }));
  Check(commits == 0);
  claim.reset();
  Check(!registry->ReserveScene(leader));
  member.Update(b, false);
  claim = registry->ReserveScene(leader);
  Check(claim.has_value());
  Check(!claim->Commit([&] {
    ++commits;
    return false;
  }));
  bool threw = false;
  try {
    claim->Commit([]() -> bool {
      throw std::runtime_error("Synthetic precommit failure");
    });
  } catch (const std::runtime_error&) {
    threw = true;
  }
  Check(threw);
  Check(claim->Commit([&] {
    ++commits;
    return true;
  }));
  Check(!claim->Commit([&] {
    ++commits;
    return true;
  }));
  Check(commits == 2 && !member.Leave());
  claim.reset();
  Check(visitor.JoinApproved(*registry->Capture(a.character_name)) ==
        party::PartyJoinStatus::kJoined);
  claim = registry->ReserveScene(leader);
  visitor.Release();
  Check(!claim->Commit([&] {
    ++commits;
    return true;
  }));
  Check(commits == 2);
  claim.reset();
  claim = registry->ReserveScene(leader);
  b.character_name = u"Renamed";
  member.Update(b, false);
  Check(!claim->Commit([&] {
    ++commits;
    return true;
  }));
  Check(commits == 2);
}

void AdoptionBarrier() {
  Fixture f;
  Sequence(f.players[0]->Process({106, {}}), {});
  Sequence(f.players[1]->Poll(), {});
  Sequence(f.players[2]->Poll(), {117});
  Sequence(f.players[0]->Poll(), {117});
  // Bob has not adopted. Even an already adopted leader cannot edit the
  // group until every connection has received its new lease and queue.
  auto denied =
      f.players[0]->Process(proto::ChangePartyLeaderRequest{u"Bob"}.Encode());
  Sequence(denied, {303});
  Check(proto::ChangePartyLeaderResponse::Decode(denied[0]).reason == 4);
  Sequence(f.players[1]->Poll(), {117});
  f.now += std::chrono::seconds(4);
  Sequence(
      f.players[0]->Process(proto::ChangePartyLeaderRequest{u"Bob"}.Encode()),
      {201, 303});
  Sequence(f.players[1]->Poll(), {201, 303});
  Sequence(f.players[2]->Poll(), {201, 303});

  Fixture dropped;
  dropped.Move({106, {}});
  Sequence(dropped.players[0]->Process(Fixture::Create().Encode()), {});
  Sequence(dropped.players[1]->Poll(), {});
  Sequence(dropped.players[2]->Poll(), {117});
  dropped.players[1].reset();
  const auto id = dropped.room_registry->FindByCharacter(100);
  Check(id && dropped.room_registry->Find(*id)->occupants.size() == 2);
  auto adopted = dropped.players[0]->Poll();
  Check(adopted.size() >= 3 && adopted[0].message_id == 102 &&
        adopted[1].message_id == 117 && adopted[2].message_id == 511);
  Check(dropped.players[0]->State() ==
        bf::sessions::PlayerSessionState::kInRoom);
  Check(!dropped.plazas->Capture(200) && !dropped.plazas->Capture(201) &&
        !dropped.plazas->Capture(202));
}

void InvitationAdoptionBarrier() {
  Fixture f;
  f.Move({106, {}});
  const auto visitor_packets = std::make_shared<rooms::RoomPacketQueue>();
  auto visitor = f.invitations->Enter({500, 600, u"Visitor", 27, 2},
                                      visitor_packets->Sink(), true);
  Sequence(f.players[0]->Process(Fixture::Create().Encode()), {});
  Sequence(f.players[1]->Poll(), {});
  Sequence(f.players[2]->Poll(), {117});
  Check(f.room_registry->Count() == 1);
  // The destination already contains Bob and Alice, but their sessions have
  // not adopted it. A previous pending Poll must not expose them as targets.
  for (const auto* name : {u"Alice", u"Bob"}) {
    const auto denied =
        visitor.RequestVisit(proto::VisitationRequest{name}, f.now);
    Sequence(denied, {123});
    Check(proto::VisitationFailedNotification::Decode(denied[0]).reason == 20);
  }
  // Carol has adopted, so room visitation remains available even though the
  // whole-party reservation is waiting for the other connections.
  Sequence(visitor.RequestVisit(proto::VisitationRequest{u"Carol"}, f.now), {});
  Sequence(f.players[2]->Poll(), {124});
  Sequence(f.players[1]->Poll(), {117});
  Sequence(visitor.RequestVisit(proto::VisitationRequest{u"Bob"}, f.now), {});
  Sequence(f.players[1]->Poll(), {124});
  Sequence(f.players[0]->Poll(), {102, 117});
  Sequence(visitor_packets->Drain(), {});
}

void CollectiveRoomExit() {
  Fixture f;
  f.Move({106, {}});
  f.Move(Fixture::Create().Encode());
  Sequence(f.players[1]->Process({404, {}}), {509});
  Check(f.room_registry->Count() == 1 && !f.plazas->Capture(201));
  f.Move({404, {}}, true);
  Check(f.room_registry->Count() == 0);
  for (std::size_t i = 0; i < 3; ++i) {
    Sequence(f.output[i], {117});
    f.Snapshot(i, 3, 3);
    Check(f.plazas->Capture(200 + i)->scene == f.plazas->Capture(200)->scene);
    Check(f.players[i]->State() ==
          bf::sessions::PlayerSessionState::kMenuInitializationSent);
  }
  f.Move(Fixture::Create().Encode());
  Check(f.room_registry->Count() == 1);
  for (std::size_t i = 0; i < 3; ++i) f.Snapshot(i, 2, 3);
  for (std::size_t i = 1; i < 3; ++i) {
    f.players[i]->Process(proto::RoomReadyRequest{1, 0, 0}.Encode());
    f.Drain();
  }
  Sequence(f.players[0]->Process(Fixture::Start().Encode()), {585, 510});
  f.Drain();
  Sequence(f.players[0]->Process({404, {}}), {509});
  Check(f.room_registry->Find(*f.room_registry->FindByCharacter(100))
            ->occupants.size() == 3);
  Check(!f.plazas->Capture(200));
}

void RoomExitRecovery() {
  Fixture f;
  f.Move({106, {}});
  f.Move(Fixture::Create().Encode());
  const auto id = *f.room_registry->FindByCharacter(100);
  const auto original = *f.room_registry->Find(id);
  Sequence(f.players[0]->Process({404, {}}), {});
  Sequence(f.players[1]->Poll(), {});
  Check(*f.room_registry->Find(id) == original);
  f.now += std::chrono::seconds(5);
  f.Clear();
  f.Drain();
  Sequence(f.output[0], {509});
  Check(*f.room_registry->Find(id) == original);
  for (const auto& player : f.players)
    Check(player->State() == bf::sessions::PlayerSessionState::kInRoom);
  Sequence(f.players[0]->Process({404, {}}), {});
  Sequence(f.players[1]->Process({803, {}}), {802, 837, 806});
  f.Clear();
  f.Drain();
  Sequence(f.output[0], {509});
  Check(*f.room_registry->Find(id) == original);
  f.faults[1] = 1;
  f.Move({404, {}});
  Sequence(f.output[0], {509});
  Check(*f.room_registry->Find(id) == original);
  for (std::size_t i = 0; i < 3; ++i) Check(!f.plazas->Capture(200 + i));
  f.faults[1] = 0;
  Sequence(f.players[0]->Process({404, {}}), {});
  Sequence(f.players[1]->Poll(), {});
  f.players[2].reset();
  f.Clear();
  f.Drain();
  Check(f.room_registry->Find(id)->occupants.size() == 2);
  Check(!f.plazas->Capture(200) && !f.plazas->Capture(201));
  f.Move({404, {}});
  Check(f.room_registry->Count() == 0);
  Check(f.plazas->Capture(200) && f.plazas->Capture(201));

  Fixture committed;
  committed.Move({106, {}});
  committed.Move(Fixture::Create().Encode());
  Sequence(committed.players[0]->Process({404, {}}), {});
  Sequence(committed.players[1]->Poll(), {});
  Sequence(committed.players[2]->Poll(), {117});
  committed.players[1].reset();
  committed.Clear();
  committed.Drain();
  Check(committed.room_registry->Count() == 0);
  Check(committed.plazas->Capture(200) && !committed.plazas->Capture(201) &&
        committed.plazas->Capture(202));
}

struct OutsideRoom {
  bf::features::characters::CharacterRecord owner{500, 600, u"Outside", 27, 2};
  std::shared_ptr<rooms::RoomPacketQueue> packets =
      std::make_shared<rooms::RoomPacketQueue>();
  rooms::RoomLease lease;
  explicit OutsideRoom(Fixture& f) {
    auto data = bf::sessions::CreateMenuSnapshot(owner, {600, 500, {}}, 0);
    auto result = rooms::RoomCreation::Create(
        *f.room_registry, {{1}, {0}, 20, 6}, Fixture::Create(),
        {600, owner, data.member, data.equipment, packets->Sink()});
    lease = std::move(result.room);
    Check(static_cast<bool>(lease));
    packets->Drain();
  }
};

void MixedSourcesAndHostSuccession() {
  Fixture f;
  OutsideRoom outside(f);
  auto inviter =
      f.invitations->Enter(outside.owner, outside.packets->Sink(), false);
  Sequence(inviter.Invite({u"Alice", 1}, f.now), {120});
  Sequence(f.players[0]->Poll(), {121});
  Sequence(f.players[0]->Process(
               proto::MoveToInviterRequest{u"Outside", 0, 0, 1}.Encode()),
           {117});
  Sequence(outside.packets->Drain(), {503});
  // The leader is in a room, Bob and Carol still have menu contexts.
  f.Move({404, {}}, true);
  for (std::size_t i = 0; i < 3; ++i) f.Snapshot(i, 3, 3);
  Sequence(outside.packets->Drain(), {511});
  Check(f.room_registry->Find(outside.lease.RoomId())->occupants.size() == 1);
  f.Move(Fixture::Create().Encode());
  const auto id = *f.room_registry->FindByCharacter(100);
  outside.lease.Release();
  auto data =
      bf::sessions::CreateMenuSnapshot(outside.owner, {600, 500, {}}, 0);
  rooms::RoomOccupant occupant;
  occupant.account_id = 600;
  occupant.character = outside.owner;
  occupant.member = data.member;
  occupant.equipment = data.equipment;
  auto joined =
      f.room_registry->Join(id, {}, occupant, outside.packets->Sink(), {});
  outside.lease = std::move(joined.room);
  Check(static_cast<bool>(outside.lease));
  outside.packets->Drain();
  f.Drain();
  outside.lease.SetMemberState({1, 0, 0});
  outside.packets->Drain();
  f.Drain();
  f.Move({404, {}});
  const auto packets = outside.packets->Drain();
  Sequence(packets, {511, 511, 511, 502});
  Check(proto::RoomHostChangedNotification::Decode(packets.back()).host_slot ==
        3);
  const auto remaining = *f.room_registry->Find(id);
  Check(remaining.occupants.size() == 1 && remaining.host_slot == 3 &&
        remaining.occupants.front().member.booleans_at96[0] == 0);
}

void DepartureAtomicity() {
  for (int iteration = 0; iteration < 8; ++iteration) {
    Fixture f;
    OutsideRoom outside(f);
    const auto c = f.stores[0]->record;
    auto data = bf::sessions::CreateMenuSnapshot(c, {200, 100, {}}, 0);
    rooms::RoomOccupant occupant;
    occupant.account_id = 200;
    occupant.character = c;
    occupant.member = data.member;
    occupant.equipment = data.equipment;
    auto queue = std::make_shared<rooms::RoomPacketQueue>();
    auto joined = f.room_registry->Join(outside.lease.RoomId(), {}, occupant,
                                        queue->Sink(), {});
    Check(static_cast<bool>(joined.room));
    joined.room.SetMemberState({1, 0, 0});
    const std::vector<rooms::RoomDeparture> leaving{{&joined.room, {200, 100}}};
    int callbacks = 0;
    const auto before = *f.room_registry->Find(outside.lease.RoomId());
    Check(!f.room_registry->DepartGroup({{&joined.room, {200, 101}}}, [&] {
      ++callbacks;
      return true;
    }));
    Check(!f.room_registry->DepartGroup({leaving[0], leaving[0]}, [&] {
      ++callbacks;
      return true;
    }));
    Check(callbacks == 0);
    auto foreign = rooms::RoomRegistry::Create(1);
    Check(!foreign->DepartGroup(leaving, [&] {
      ++callbacks;
      return true;
    }));
    Check(callbacks == 0);
    Check(!f.room_registry->DepartGroup(leaving, [] { return false; }));
    bool threw = false;
    try {
      f.room_registry->DepartGroup(leaving, []() -> bool {
        throw std::runtime_error("Synthetic destination failure");
      });
    } catch (const std::runtime_error&) {
      threw = true;
    }
    Check(threw && *f.room_registry->Find(outside.lease.RoomId()) == before);
    std::barrier gate(3);
    bool departed = false;
    rooms::RoomStartStatus started{};
    std::exception_ptr start_error, departure_error;
    std::thread departure([&] {
      gate.arrive_and_wait();
      try {
        departed = f.room_registry->DepartGroup(leaving, [] { return true; });
      } catch (...) {
        departure_error = std::current_exception();
      }
    });
    std::thread start([&] {
      gate.arrive_and_wait();
      try {
        started =
            rooms::RoomActions::Start(outside.lease, {2}, Fixture::Start());
      } catch (...) {
        start_error = std::current_exception();
      }
    });
    gate.arrive_and_wait();
    departure.join();
    start.join();
    Check(!start_error && !departure_error);
    Check(departed ? started == rooms::RoomStartStatus::kNotEnoughPlayers
                   : started == rooms::RoomStartStatus::kStarted);
    Check(static_cast<bool>(joined.room) == !departed);
    Check(f.room_registry->Find(outside.lease.RoomId())->occupants.size() ==
          (departed ? 1U : 2U));
    if (!departed) {
      outside.lease.ReturnToWaiting();
      Check(f.room_registry->DepartGroup(leaving, [] { return true; }));
    } else {
      Check(!joined.room);
    }
    // A consumed handle cannot remove the same identity after it joins again.
    auto replacement = f.room_registry->Join(outside.lease.RoomId(), {},
                                             occupant, queue->Sink(), {});
    Check(static_cast<bool>(replacement.room));
    joined.room.Release();
    Check(f.room_registry->Find(outside.lease.RoomId())->occupants.size() == 2);
  }
}

void MultipleSourceRooms() {
  Fixture f;
  std::array<rooms::RoomLease, 2> leases;
  std::array<std::shared_ptr<rooms::RoomPacketQueue>, 2> queues;
  std::vector<rooms::RoomDeparture> leaving;
  for (std::size_t i = 0; i < 2; ++i) {
    const auto& c = f.stores[i]->record;
    auto data = bf::sessions::CreateMenuSnapshot(
        c, {c.account_id, c.character_id, {}}, 0);
    queues[i] = std::make_shared<rooms::RoomPacketQueue>();
    auto result = rooms::RoomCreation::Create(
        *f.room_registry, {{1}, {0}, 20, 6}, Fixture::Create(),
        {c.account_id, c, data.member, data.equipment, queues[i]->Sink()});
    leases[i] = std::move(result.room);
    Check(static_cast<bool>(leases[i]));
    leaving.push_back({&leases[i], {c.account_id, c.character_id}});
    queues[i]->Drain();
  }
  Check(rooms::RoomActions::Start(leases[1], {}, Fixture::Start()) ==
        rooms::RoomStartStatus::kStarted);
  int admitted = 0;
  Check(!f.room_registry->DepartGroup(leaving, [&] {
    ++admitted;
    return true;
  }));
  Check(admitted == 0 && leases[0] && leases[1]);
  Check(f.room_registry->Count() == 2 &&
        f.room_registry->Find(leases[0].RoomId())->state ==
            rooms::RoomState::kWaiting);
  leases[1].ReturnToWaiting();
  Check(f.room_registry->DepartGroup(leaving, [&] {
    ++admitted;
    return true;
  }));
  Check(admitted == 1 && !leases[0] && !leases[1]);
  Check(f.room_registry->Count() == 0);
}
}  // namespace
int RunPartySceneTests() {
  WholePartyGame();
  CancellationAndRecovery();
  DisconnectAndMalformedActor();
  JoiningAndPolicyRefusal();
  ReservationAuthority();
  AdoptionBarrier();
  InvitationAdoptionBarrier();
  std::cout << "Party scene: " << checks << " checks passed\n";
  return checks;
}

int RunPartyRoomExitTests() {
  checks = 0;
  CollectiveRoomExit();
  RoomExitRecovery();
  MixedSourcesAndHostSuccession();
  DepartureAtomicity();
  MultipleSourceRooms();
  std::cout << "Party room exit: " << checks << " checks passed\n";
  return checks;
}

int RunChannelSceneTests() {
  checks = 0;
  WholePartyGame(true);
  std::cout << "Channel refusal scene continuity: " << checks
            << " checks passed\n";
  return checks;
}
