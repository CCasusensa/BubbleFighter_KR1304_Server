#include <algorithm>
#include <array>
#include <barrier>
#include <iostream>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "features/rooms/room_actions.h"
#include "features/rooms/room_map_catalog.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/rooms/room_list_request.h"
#include "protocol/scenes/channel_transfer.h"
#include "protocol/scenes/move_into_response.h"
#include "sessions/connection_handoff.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bf = bubble_fighter::server;
namespace proto = bf::protocol;
namespace channels = bf::features::channels;
namespace sessions = bf::sessions;
namespace {
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Party channel line " + std::to_string(at.line()));
}
using Result = bf::network::PacketHandlingResult;
void Ids(const Result& result, std::initializer_list<std::uint16_t> ids,
         std::source_location at = std::source_location::current()) {
  if (result.close_connection || result.responses.size() != ids.size() ||
      !std::equal(result.responses.begin(), result.responses.end(), ids.begin(),
                  [](const auto& packet, auto id) {
                    return packet.message_id == id;
                  })) {
    std::ostringstream message;
    message << "Party channel packet order at " << at.line() << ": got";
    for (const auto& packet : result.responses)
      message << ' ' << packet.message_id;
    message << " closed=" << result.close_connection << " expected";
    for (auto id : ids) message << ' ' << id;
    throw std::runtime_error(message.str());
  }
  checks += 2;
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
  using Connection = sessions::HandoffConnectionHandlers;
  using Session = sessions::PlayerSession;
  sessions::ConnectionHandoffs::Clock::time_point now{};
  std::shared_ptr<bf::features::parties::PartyRegistry> parties =
      std::make_shared<bf::features::parties::PartyRegistry>(8);
  std::shared_ptr<channels::ChannelWorlds> worlds;
  std::shared_ptr<sessions::ConnectionHandoffs> handoffs =
      std::make_shared<sessions::ConnectionHandoffs>([this] { return now; });
  std::array<std::shared_ptr<Characters>, 3> stores;
  std::array<std::weak_ptr<Session>, 3> sessions;
  std::array<Connection, 3> sources, destinations;
  std::array<proto::HandoffGrantNotification, 3> grants;
  bool bad_equipment = false;
  explicit Fixture(bool enable = true) {
    std::istringstream rules(
        "[20]\nMinimumLevel=1\nMaximumLevel=20\n[70]\nMinimumLevel=21\nMaximumLevel=260\n");
    worlds = std::make_shared<channels::ChannelWorlds>(
        channels::ChannelRules::Read(rules), parties);
    handoffs->SetEndpoint({127, 0, 0, 1}, 17110);
    const std::array<std::u16string, 3> names{u"Alice", u"Bob", u"Carol"};
    auto world = worlds->Find(60);
    for (std::size_t i = 0; i < sources.size(); ++i) {
      stores[i] = std::make_shared<Characters>();
      stores[i]->record = {100 + i, 200 + i, names[i], 27, 2};
      auto store = stores[i];
      auto player = std::make_shared<Session>(
          [store](std::string_view) {
            return std::optional{store->record.account_id};
          },
          store,
          [this](const auto& c, auto observed) {
            auto result = bf::sessions::CreateMenuSnapshot(
                c, {c.account_id, c.character_id, {}}, observed);
            if (bad_equipment && c.account_id == 201) ++result.equipment.key;
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
      player->Process(hello.Encode());
      proto::LoginRequest login;
      login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
      player->Process(login.Encode());
      player->Process({521, {}});
      player->Process({106, {}});
      sessions[i] = player;
      sources[i] = Attach(player);
      if (enable || i != 2)
        sources[i].handlers.on_packet(
            proto::HandoffCapabilityRequest{}.Encode());
    }
    Drain();
  }
  Connection Attach(std::shared_ptr<Session> player = {}) {
    if (!player)
      return handoffs->Attach(
          {[](const auto&) { return Result{}; }, [] { return Result{}; }},
          {[] { return std::optional<sessions::HandoffIdentity>{}; },
           [](auto) { return false; }, [] {}});
    auto group = std::make_shared<sessions::HandoffSessionHooks::Group>();
    group->enable = [player] { player->EnableChannelHandoff(); };
    group->begin = [player](auto category) {
      return player->BeginPartyChannelHandoff(category);
    };
    group->poll = [player] { return player->PollPartyChannelHandoff(); };
    group->signal = [player](auto signal) {
      return player->SignalPartyChannelHandoff(signal);
    };
    group->cancel = [player] { player->CancelPartyChannelHandoff(); };
    group->irreversible = [player] {
      return player->PartyChannelIrreversible();
    };
    return handoffs->Attach(
        {[player](const auto& packet) {
           return Result{player->Process(packet), false};
         },
         [player] { return Result{player->Poll(), false}; }},
        {[player] {
           return std::optional{sessions::HandoffIdentity{
               *player->AccountId(),
               static_cast<std::uint32_t>(player->Character()->character_id)}};
         },
         [player](auto category) {
           return player->BeginConnectionHandoff(category);
         },
         [player] { player->EndConnectionHandoff(); },
         [player] { return player->CompleteConnectionHandoff(); }, group});
  }
  void Drain() {
    for (int n = 0; n < 4; ++n)
      for (auto& c : sources) c.handlers.on_tick();
  }
  void Party() {
    for (std::size_t i = 1; i < sources.size(); ++i) {
      now += std::chrono::seconds(4);
      sources[0].handlers.on_packet(
          proto::InvitationRequest{stores[i]->record.character_name, 1}
              .Encode());
      Drain();
      sources[i].handlers.on_packet(
          proto::MoveToInviterRequest{u"Alice", 0, 0, 1}.Encode());
      Drain();
    }
    auto target = parties->Capture(u"Alice");
    Check(target.has_value());
    Check(parties->Capture(u"Bob")->party_id == target->party_id &&
          parties->Capture(u"Carol")->party_id == target->party_id);
  }
  void Begin() {
    Ids(sources[0].handlers.on_packet(proto::MoveChannelRequest{20}.Encode()),
        {});
    std::array<bool, 3> offered{};
    for (int pass = 0; pass < 4; ++pass)
      for (std::size_t i = 0; i < 3; ++i) {
        auto result = sources[i].handlers.on_tick();
        if (!result.responses.empty()) {
          Ids(result, {65021, 8});
          Check(!offered[i]);
          offered[i] = true;
          grants[i] =
              proto::HandoffGrantNotification::Decode(result.responses[0]);
          Check(grants[i].character_id == stores[i]->record.character_id &&
                grants[i].channel_category == 20);
        }
      }
    Check(
        std::all_of(offered.begin(), offered.end(), [](bool v) { return v; }));
    Check(grants[0].secret != grants[1].secret &&
          grants[1].secret != grants[2].secret);
  }
  Result Enter(std::size_t i) {
    destinations[i] = Attach();
    Ids(destinations[i].handlers.on_packet(
            proto::HandoffProofRequest{grants[i].character_id, grants[i].secret}
                .Encode()),
        {});
    return destinations[i].handlers.on_packet(
        proto::MoveIntoRequest{grants[i].character_id, 1}.Encode());
  }
  void EnterAll() {
    Ids(Enter(0), {});
    Ids(Enter(1), {});
    Ids(destinations[0].handlers.on_tick(), {});
    Ids(Enter(2), {9});
    Ids(destinations[0].handlers.on_tick(), {9});
    Ids(destinations[1].handlers.on_tick(), {9});
  }
  Result Ack(std::size_t i) {
    return destinations[i].handlers.on_packet(
        proto::ChannelSwitchAcknowledgementRequest{}.Encode());
  }
  void SourceIntact() {
    for (std::size_t i = 0; i < 3; ++i) {
      Check(worlds->Find(60)->plazas->Capture(200 + i).has_value());
      Check(!worlds->Find(20)->plazas->Capture(200 + i));
    }
  }
};
void CommitAndReuse() {
  Fixture f;
  f.Party();
  f.Begin();
  f.SourceIntact();
  f.EnterAll();
  Ids(f.Ack(1), {});
  Ids(f.Ack(0), {});
  f.SourceIntact();
  Ids(f.destinations[0].handlers.on_tick(), {});
  Ids(f.Ack(2), {10, 11, 117});
  Ids(f.destinations[0].handlers.on_tick(), {10, 11, 117});
  Ids(f.destinations[1].handlers.on_tick(), {10, 11, 117});
  const auto scene = f.worlds->Find(20)->plazas->Capture(200)->scene;
  for (std::size_t i = 0; i < 3; ++i) {
    Check(!f.worlds->Find(60)->plazas->Capture(200 + i));
    Check(f.worlds->Find(20)->plazas->Capture(200 + i)->scene == scene);
    Check(f.sessions[i].lock()->ChannelCategory() == 20);
    Check(f.sources[i].handlers.on_tick().close_connection);
    Check(f.destinations[i].authenticated());
    auto replay = f.Attach();
    bool rejected = false;
    try {
      replay.handlers.on_packet(proto::HandoffProofRequest{
          f.grants[i].character_id, f.grants[i].secret}
                                    .Encode());
    } catch (const std::exception&) {
      rejected = true;
    }
    Check(rejected);
    f.sources[i] = std::move(f.destinations[i]);
  }
  f.Drain();
  // Reuse the party in the target world: the leader creates one room for all.
  proto::CreateRoomRequest create;
  create.settings.text_at8 = u"Together";
  create.settings.configuration.short_at4 = 1;
  f.sources[0].handlers.on_packet(create.Encode());
  f.Drain();
  for (auto& session : f.sessions)
    Check(session.lock()->State() == sessions::PlayerSessionState::kInRoom);
  const auto list = f.worlds->Find(20)->rooms->List(0, 10);
  Check(list.rooms.size() == 1);
}
void EarlyFailure() {
  for (int kind = 0; kind < 4; ++kind) {
    Fixture f(kind != 0);
    f.Party();
    if (kind == 1) f.stores[1]->record.statistics.level = 21;
    if (kind == 2) f.bad_equipment = true;
    if (kind == 3) {
      const auto result = f.sources[1].handlers.on_packet(
          proto::MoveChannelRequest{20}.Encode());
      Ids(result, {8});
      Check(proto::MoveChannelResponse::Decode(result.responses[0]).result ==
            2);
    } else {
      auto initial = f.sources[0].handlers.on_packet(
          proto::MoveChannelRequest{20}.Encode());
      bool refused = !initial.responses.empty();
      for (int pass = 0; pass < 4; ++pass)
        for (auto& c : f.sources) {
          const auto result = c.handlers.on_tick();
          for (const auto& packet : result.responses)
            if (packet.message_id == 8) {
              refused = true;
              Check(proto::MoveChannelResponse::Decode(packet).result == 2);
            }
        }
      Check(refused);
    }
    f.SourceIntact();
    for (auto& c : f.sources) Check(c.authenticated());
  }
}
void CancellationAndDisconnection() {
  for (int kind = 0; kind < 4; ++kind) {
    Fixture f;
    f.Party();
    f.Begin();
    if (kind == 0) {
      Ids(f.Enter(0), {});
      f.destinations[0] = {};
    }
    if (kind == 1) f.now += std::chrono::seconds(11);
    if (kind == 2)
      f.sources[1].handlers.on_packet(proto::RoomListRequest{}.Encode());
    if (kind == 3) {
      f.EnterAll();
      f.destinations[0] = {};
    }
    for (int pass = 0; pass < 4; ++pass)
      for (auto& c : f.sources) c.handlers.on_tick();
    if (kind != 3) {
      f.SourceIntact();
      for (auto& c : f.sources) Check(c.authenticated());
      f.Begin();  // Cancelled operations release every reservation/target
                  // registration.
    } else {
      for (auto& c : f.sources) Check(c.handlers.on_tick().close_connection);
      for (std::size_t i = 0; i < 3; ++i) {
        Check(f.sessions[i].expired());
        Check(!f.worlds->Find(60)->plazas->Capture(200 + i));
        Check(!f.worlds->Find(20)->plazas->Capture(200 + i));
      }
    }
  }
}
void CommittedDisconnect() {
  Fixture f;
  f.Party();
  f.Begin();
  f.EnterAll();
  Ids(f.Ack(0), {});
  Ids(f.Ack(1), {});
  Ids(f.Ack(2), {10, 11, 117});
  f.destinations[2] = {};
  Check(f.sessions[2].expired());
  Ids(f.destinations[0].handlers.on_tick(), {10, 11, 117, 511, 501});
  Ids(f.destinations[1].handlers.on_tick(), {10, 11, 117, 511, 501});
  Check(f.worlds->Find(20)->plazas->Capture(200).has_value());
  Check(f.worlds->Find(20)->plazas->Capture(201).has_value());
  Check(!f.worlds->Find(20)->plazas->Capture(202));
}
void ConcurrentConfirmations() {
  for (int trial = 0; trial < 8; ++trial) {
    Fixture f;
    f.Party();
    f.Begin();
    f.EnterAll();
    std::barrier gate(4);
    std::array<Result, 3> results;
    std::array<std::exception_ptr, 3> errors;
    std::array<std::thread, 3> threads;
    for (std::size_t i = 0; i < 3; ++i)
      threads[i] = std::thread([&, i] {
        gate.arrive_and_wait();
        try {
          results[i] = f.Ack(i);
        } catch (...) {
          errors[i] = std::current_exception();
        }
      });
    gate.arrive_and_wait();
    for (auto& thread : threads) thread.join();
    for (std::size_t i = 0; i < 3; ++i) {
      Check(!errors[i]);
      if (results[i].responses.empty())
        results[i] = f.destinations[i].handlers.on_tick();
      Ids(results[i], {10, 11, 117});
      Check(f.sessions[i].lock()->ChannelCategory() == 20);
    }
  }
}
void WaitingRoomStartRace() {
  namespace rooms = bf::features::rooms;
  for (int trial = 0; trial < 10; ++trial) {
    Fixture f;
    f.Party();
    const auto world = f.worlds->Find(60);
    const bf::features::characters::CharacterRecord outsider{999, 999,
                                                             u"Outside", 27, 2};
    const auto data =
        sessions::CreateMenuSnapshot(outsider, {999, 999, {}}, 12345);
    auto packets = std::make_shared<rooms::RoomPacketQueue>();
    proto::CreateRoomRequest create;
    create.settings.text_at8 = u"Race";
    create.settings.configuration.short_at4 = 1;
    auto outside = rooms::RoomCreation::Create(
        *world->rooms, {{1}, {0}, 20, 6}, create,
        {999, outsider, data.member, data.equipment, packets->Sink()});
    Check(static_cast<bool>(outside.room));
    const auto room_id = outside.room.RoomId();
    f.sources[0].handlers.on_packet(
        proto::JoinRoomRequest{room_id, {}}.Encode());
    f.Drain();
    for (auto& source : f.sources)
      source.handlers.on_packet(proto::RoomReadyRequest{1, 0, 0}.Encode());
    f.Drain();
    f.Begin();
    f.EnterAll();
    Ids(f.Ack(0), {});
    Ids(f.Ack(1), {});
    const auto& map = rooms::RoomMapCatalog::Maps().front();
    proto::StartGameRequest start{0,
                                  std::u16string(map.directory),
                                  std::u16string(map.name),
                                  std::u16string(map.key),
                                  map.difficulty,
                                  map.rules,
                                  0};
    std::barrier gate(3);
    Result committed;
    rooms::RoomStartStatus started{};
    std::exception_ptr commit_error, start_error;
    auto commit = [&] {
      try {
        committed = f.Ack(2);
      } catch (...) {
        commit_error = std::current_exception();
      }
    };
    auto begin_game = [&] {
      try {
        started = rooms::RoomActions::Start(outside.room, {2}, start);
      } catch (...) {
        start_error = std::current_exception();
      }
    };
    if (trial == 0) {
      begin_game();
      commit();
    } else if (trial == 1) {
      commit();
      begin_game();
    } else {
      std::thread a([&] {
        gate.arrive_and_wait();
        commit();
      });
      std::thread b([&] {
        gate.arrive_and_wait();
        begin_game();
      });
      gate.arrive_and_wait();
      a.join();
      b.join();
    }
    Check(!commit_error && !start_error);
    const bool moved = !committed.close_connection;
    Check(moved ? started == rooms::RoomStartStatus::kNotEnoughPlayers
                : started == rooms::RoomStartStatus::kStarted);
    if (moved) {
      Ids(committed, {10, 11, 117});
      for (std::size_t i = 0; i < 2; ++i)
        Ids(f.destinations[i].handlers.on_tick(), {10, 11, 117});
    } else {
      for (auto& source : f.sources)
        Check(source.handlers.on_tick().close_connection);
      Check(committed.close_connection);
      Check(f.destinations[0].handlers.on_tick().close_connection);
      Check(f.destinations[1].handlers.on_tick().close_connection);
    }
    Check(world->rooms->Find(room_id)->occupants.size() == 1);
    for (std::size_t i = 0; i < 3; ++i) {
      Check(f.worlds->Find(20)->plazas->Capture(200 + i).has_value() == moved);
      Check(f.sessions[i].expired() == !moved);
    }
  }
}
}  // namespace
int RunPartyChannelTests() {
  CommitAndReuse();
  EarlyFailure();
  CancellationAndDisconnection();
  CommittedDisconnect();
  ConcurrentConfirmations();
  WaitingRoomStartRace();
  std::cout << "Party channel: " << checks << " checks passed\n";
  return checks;
}
