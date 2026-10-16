#include <array>
#include <barrier>
#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "features/rooms/room_actions.h"
#include "features/rooms/room_creation.h"
#include "features/rooms/room_map_catalog.h"
#include "features/rooms/room_packet_queue.h"
#include "protocol/battle/game_loading_complete_request.h"
#include "protocol/battle/game_movement_notification.h"
#include "protocol/battle/game_start_notification.h"
#include "sessions/menu_snapshot.h"

namespace bf = bubble_fighter::server;
namespace rooms = bf::features::rooms;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Room admission line " +
                             std::to_string(at.line()));
}
template <class Action>
void Rejects(Action action) {
  bool failed = false;
  try {
    action();
  } catch (const std::exception&) {
    failed = true;
  }
  Check(failed);
}
void Sequence(const std::vector<proto::TcpPacket>& packets,
              std::initializer_list<std::uint16_t> ids) {
  Check(packets.size() == ids.size());
  auto at = ids.begin();
  for (const auto& packet : packets) Check(packet.message_id == *at++);
}
struct Fixture {
  rooms::RoomGameSession::Time now{};
  std::shared_ptr<rooms::RoomRegistry> registry = rooms::RoomRegistry::Create(
      4, [this] { return now; }, [] { return 12345; });
  std::array<std::shared_ptr<rooms::RoomPacketQueue>, 24> queues;
  std::array<rooms::RoomLease, 24> leases;
  Fixture() {
    for (auto& q : queues) q = std::make_shared<rooms::RoomPacketQueue>();
  }
  static bf::features::characters::CharacterRecord Character(std::size_t i) {
    return {100 + i, 200 + i,
            u"Peer" + std::u16string(1, static_cast<char16_t>(u'A' + i)), 10,
            7};
  }
  rooms::RoomCreationInput Input(std::size_t i) {
    auto character = Character(i);
    auto data = bf::sessions::CreateMenuSnapshot(
        character, {character.account_id, character.character_id, {}}, 1000);
    return {character.account_id, character, std::move(data.member),
            std::move(data.equipment), queues.at(i)->Sink()};
  }
  rooms::RoomAdmissionEntry Entry(std::size_t i) {
    auto input = Input(i);
    return {{0, input.authenticated_account_id, std::move(input.character),
             std::move(input.member), std::move(input.equipment)},
            std::move(input.sink),
            {}};
  }
  static proto::CreateRoomRequest Request() {
    proto::CreateRoomRequest request;
    request.settings.text_at8 = u"Together";
    request.settings.configuration.short_at4 = 1;
    return request;
  }
  static rooms::RoomCreationPolicy Policy() { return {{1}, {0}, 20, 6}; }
  rooms::RoomGroupCreationOutcome Create(
      std::initializer_list<std::size_t> ids) {
    std::vector<rooms::RoomCreationInput> inputs;
    for (auto i : ids) inputs.push_back(Input(i));
    return rooms::RoomCreation::CreateGroup(*registry, Policy(), Request(),
                                            std::move(inputs));
  }
  void Adopt(std::vector<rooms::RoomLease> members,
             std::initializer_list<std::size_t> ids) {
    Check(members.size() == ids.size());
    std::size_t at = 0;
    for (auto i : ids) leases.at(i) = std::move(members.at(at++));
  }
  rooms::RoomGroupJoinResult Join(std::initializer_list<std::size_t> ids) {
    std::vector<rooms::RoomAdmissionEntry> entries;
    for (auto i : ids) entries.push_back(Entry(i));
    return registry->JoinGroup(leases[0].RoomId(), {}, std::move(entries));
  }
  void Clear() {
    for (auto& q : queues) q->Clear();
  }
  void Empty() {
    for (auto& q : queues) Check(q->Drain().empty());
  }
  void Snapshot(std::size_t i, std::uint32_t slot, std::uint32_t count,
                bool creator = false) {
    auto packets = queues[i]->Drain();
    if (creator)
      Sequence(packets, {102, 117});
    else
      Sequence(packets, {117});
    auto scene = proto::GotoRoomNotification::Decode(packets.back());
    Check(scene.descriptor.session_type == 2 &&
          scene.descriptor.value_at4 == 2);
    Check(scene.value_at728 == slot && scene.value_at544 == 0);
    Check(scene.members.size() == count &&
          scene.descriptor.shorts_at32[0] == count);
    Check(scene.members[slot].value_at8 == Character(i).character_id);
    Check(scene.members[slot].text_at12 == Character(i).character_name);
    Check(scene.records_at564.size() == count &&
          scene.records_at588.size() == count);
    for (std::uint32_t j = 0; j < count; ++j) {
      Check(scene.members[j].booleans_at0[0] == 1);
      Check(scene.members[j].booleans_at96 == std::array<std::uint32_t, 3>{});
      Check(scene.records_at564[j].key == j);
      Check(scene.records_at588[j].key == scene.members[j].value_at8);
    }
  }
  static proto::StartGameRequest StartRequest() {
    const auto& map = rooms::RoomMapCatalog::Maps().front();
    return {0,
            std::u16string(map.directory),
            std::u16string(map.name),
            std::u16string(map.key),
            map.difficulty,
            map.rules,
            0};
  }
};

void CreationAndRollback() {
  Fixture f;
  auto result = f.Create({0, 1, 2});
  Check(result.result == proto::CreateRoomResult::kCreated &&
        result.responses.empty());
  f.Adopt(std::move(result.members), {0, 1, 2});
  for (std::size_t i = 0; i < 3; ++i)
    f.Snapshot(i, static_cast<std::uint32_t>(i), 3, i == 0);
  auto record = *f.registry->Find(f.leases[0].RoomId());
  Check(record.occupants[0].member.signed_byte_at4 == 1);
  Check(record.occupants[1].member.signed_byte_at4 == 2);
  Check(record.occupants[2].member.signed_byte_at4 == 1);
  Check(record.settings.configuration.texts_at44[0] == u"Random");
  auto refused = f.Create({3, 1});
  Check(refused.result == proto::CreateRoomResult::kFailed &&
        refused.members.empty());
  Sequence(refused.responses, {102});
  Check(!f.registry->FindByCharacter(Fixture::Character(3).character_id));
  Check(*f.registry->Find(f.leases[0].RoomId()) == record);
  f.Empty();
  auto a = f.Input(3), b = f.Input(4);
  b.equipment.groups[0].resize(1025);
  Rejects([&] {
    rooms::RoomCreation::CreateGroup(*f.registry, Fixture::Policy(),
                                     Fixture::Request(), {a, b});
  });
  Check(f.registry->Count() == 1);
  Check(!f.registry->FindByCharacter(Fixture::Character(3).character_id));
  Check(!f.registry->FindByCharacter(Fixture::Character(4).character_id));
  f.Empty();
  auto retry = f.Create({3, 4});
  Check(retry.members.size() == 2 && f.registry->Count() == 2);
  f.Snapshot(3, 0, 2, true);
  f.Snapshot(4, 1, 2);
  retry.members.clear();
  Check(f.registry->Count() == 1);
  f.leases[0].Release();
  Sequence(f.queues[1]->Drain(), {511, 502});
  Check(f.registry->Find(record.room_id)->host_slot == 1);
  f.leases[1].Release();
  f.leases[2].Release();
  Check(f.registry->Count() == 0);
}

void Reservations() {
  Fixture f;
  const auto identity = [](std::size_t i) {
    auto c = Fixture::Character(i);
    return rooms::RoomIdentity{c.account_id, c.character_id};
  };
  Rejects([&] { f.registry->ReserveGroup({}); });
  Rejects([&] { f.registry->ReserveGroup({identity(0), identity(0)}); });
  Rejects([&] { f.registry->ReserveGroup({{0, 1}}); });
  Rejects([&] { f.registry->ReserveGroup({{1, 0x100000000ULL}}); });
  std::vector<rooms::RoomIdentity> many;
  for (std::size_t i = 0; i < 17; ++i) many.push_back(identity(i));
  Rejects([&] { f.registry->ReserveGroup(many); });
  auto claim = f.registry->ReserveGroup({identity(0), identity(1)});
  Check(claim.status == rooms::RoomReserveStatus::kReserved);
  Check(f.registry->Count() == 0 &&
        !f.registry->Find(claim.reservation->RoomId()));
  Check(f.registry->List(0, 10).rooms.empty());
  Check(f.registry->ReserveGroup({identity(2), identity(1)}).status ==
        rooms::RoomReserveStatus::kAlreadyInRoom);
  Check(!f.registry->FindByCharacter(identity(2).character_id));
  rooms::RoomRecord record;
  record.room_id = claim.reservation->RoomId();
  record.slot_count = 2;
  record.settings = Fixture::Request().settings;
  auto first = f.Entry(0), second = f.Entry(1);
  second.occupant.slot = 1;
  record.occupants = {first.occupant, second.occupant};
  Rejects(
      [&] { std::move(*claim.reservation).Commit(record, first.sink, {}); });
  Check(f.registry->Reserve(identity(1).account_id, identity(1).character_id)
            .status == rooms::RoomReserveStatus::kAlreadyInRoom);
  auto bad = record;
  bad.occupants[1].slot = 0;
  const std::vector<rooms::RoomPublication> deliveries{
      {first.occupant.account_id, first.sink, {}},
      {second.occupant.account_id, second.sink, {}}};
  Rejects([&] { std::move(*claim.reservation).CommitGroup(bad, deliveries); });
  bad = record;
  bad.occupants[1].equipment.groups[0].resize(1025);
  Rejects([&] { std::move(*claim.reservation).CommitGroup(bad, deliveries); });
  bad = record;
  bad.occupants[1].slot = 1000000;
  bool bounds_rejected = false;
  try {
    std::move(*claim.reservation).CommitGroup(bad, deliveries);
  } catch (const std::invalid_argument&) {
    bounds_rejected = true;
  }
  Check(bounds_rejected);
  f.Empty();
  auto members = std::move(*claim.reservation).CommitGroup(record, deliveries);
  Check(members.size() == 2 && f.registry->Count() == 1);
  f.Empty();  // Caller-provided empty publication remains empty.
  Rejects(
      [&] { std::move(*claim.reservation).CommitGroup(record, deliveries); });
  members.clear();
  auto abandoned = f.registry->ReserveGroup({identity(0), identity(1)});
  abandoned.reservation.reset();
  auto reused = f.registry->ReserveGroup({identity(0), identity(1)});
  Check(reused.status == rooms::RoomReserveStatus::kReserved);
  reused.reservation.reset();
  f.registry = rooms::RoomRegistry::Create(1);
  auto only = f.registry->ReserveGroup({identity(0), identity(1)});
  Check(f.registry->ReserveGroup({identity(2), identity(3)}).status ==
        rooms::RoomReserveStatus::kRegistryFull);
  only.reservation.reset();
  Check(f.registry->ReserveGroup({identity(2), identity(3)}).status ==
        rooms::RoomReserveStatus::kReserved);
}

void JoinAndTarget() {
  Fixture f;
  f.Adopt(f.Create({0, 1}).members, {0, 1});
  f.Clear();
  const auto target =
      *f.registry->InvitationTarget(Fixture::Character(0).character_id);
  auto before = *f.registry->Find(target.room_id);
  auto entries = std::vector{f.Entry(2), f.Entry(3)};
  entries[1].occupant.equipment.groups[0].resize(1025);
  Rejects([&] { f.registry->JoinGroup(target.room_id, {}, entries); });
  Check(*f.registry->Find(target.room_id) == before);
  Check(!f.registry->FindByCharacter(Fixture::Character(2).character_id));
  f.Empty();
  auto wrong = target;
  ++wrong.epoch;
  Check(f.registry->JoinGroupInvited(wrong, {f.Entry(2), f.Entry(3)})
            .members.empty());
  wrong = target;
  wrong.inviter_name = u"Wrong";
  Check(f.registry->JoinGroupInvited(wrong, {f.Entry(2), f.Entry(3)})
            .members.empty());
  Check(
      f.registry->JoinGroup(target.room_id, u"Wrong", {f.Entry(2), f.Entry(3)})
          .status == rooms::RoomJoinStatus::kWrongPassword);
  f.Empty();
  entries = {f.Entry(2), f.Entry(3)};
  entries[0].prefix.push_back(proto::JoinRoomResponse{}.Encode());
  entries[1].occupant.member.booleans_at96 = {1, 1, 1};
  auto joined = f.registry->JoinGroupInvited(target, entries);
  Check(joined.status == rooms::RoomJoinStatus::kJoined);
  f.Adopt(std::move(joined.members), {2, 3});
  Sequence(f.queues[0]->Drain(), {503, 503});
  Sequence(f.queues[1]->Drain(), {503, 503});
  Sequence(f.queues[2]->Drain(), {103, 117});
  f.Snapshot(3, 3, 4);
  Check(
      f.registry->InvitationTarget(Fixture::Character(0).character_id)->epoch ==
      target.epoch);
  Rejects([&] { f.Join({4, 2}); });
  Check(!f.registry->FindByCharacter(Fixture::Character(4).character_id));
  f.Empty();
  f.leases[1].Release();
  f.leases[3].Release();
  f.Clear();
  Check(f.registry->JoinGroupInvited(target, {f.Entry(4), f.Entry(5)})
            .members.empty());
  auto holes = f.Join({4, 5});
  f.Adopt(std::move(holes.members), {4, 5});
  f.Snapshot(4, 1, 4);
  f.Snapshot(5, 3, 4);
  Check(f.registry->Find(target.room_id)->occupants[1].character.character_id ==
        Fixture::Character(4).character_id);
}

void GroupGame() {
  Fixture f;
  f.Adopt(f.Create({0}).members, {0});
  f.Adopt(f.Join({1, 2}).members, {1, 2});
  f.Clear();
  Check(rooms::RoomActions::Start(f.leases[0], {}, Fixture::StartRequest()) ==
        rooms::RoomStartStatus::kMembersNotReady);
  f.leases[1].SetMemberState({1, 0, 0});
  Check(rooms::RoomActions::Start(f.leases[0], {}, Fixture::StartRequest()) ==
        rooms::RoomStartStatus::kMembersNotReady);
  f.leases[2].SetMemberState({1, 0, 0});
  f.Clear();
  Check(rooms::RoomActions::Start(f.leases[0], {}, Fixture::StartRequest()) ==
        rooms::RoomStartStatus::kStarted);
  for (std::size_t i = 0; i < 3; ++i)
    Sequence(f.queues[i]->Drain(), {585, 510});
  Check(f.Join({3, 4}).status == rooms::RoomJoinStatus::kPlaying);
  for (std::size_t i = 0; i < 2; ++i) {
    f.leases[i].ProcessGame(proto::GameLoadingCompleteRequest{}.Encode());
    Sequence(f.queues[i]->Drain(), {606});
    f.Empty();
  }
  f.leases[2].ProcessGame(proto::GameLoadingCompleteRequest{}.Encode());
  Sequence(f.queues[0]->Drain(), {607});
  Sequence(f.queues[1]->Drain(), {607});
  Sequence(f.queues[2]->Drain(), {606, 607});
  f.now += std::chrono::milliseconds(3999);
  f.leases[0].Poll();
  f.Empty();
  f.now += std::chrono::milliseconds(1);
  f.leases[0].Poll();
  for (std::size_t i = 0; i < 3; ++i) {
    const auto packets = f.queues[i]->Drain();
    Sequence(packets, {608});
    const auto start = proto::GameStartNotification::Decode(packets[0]);
    Check(start.random_seed == 12345 && start.duration_milliseconds == 240000);
  }
  Check(f.registry->Find(f.leases[0].RoomId())->state ==
        rooms::RoomState::kPlaying);
  auto move = proto::GameMovementNotification{1200000, {1, 2, 3}}.Encode();
  f.leases[2].ProcessGame(move);
  for (std::size_t i = 0; i < 2; ++i) {
    auto packets = f.queues[i]->Drain();
    Sequence(packets, {0x8075});
    Check(packets[0].payload == move.payload);
  }
  Check(f.queues[2]->Drain().empty());
  f.leases[0].ReturnToWaiting();
  for (std::size_t i = 0; i < 3; ++i)
    Sequence(f.queues[i]->Drain(), {620, 117});
  Check(f.registry->Find(f.leases[0].RoomId())->state ==
        rooms::RoomState::kWaiting);
}

void CapacityAndRaces() {
  for (int round = 0; round < 10; ++round) {
    Fixture f;
    f.Adopt(f.Create({0}).members, {0});
    std::vector<rooms::RoomAdmissionEntry> initial;
    // The ordinary room hosts 12 seats: ten occupants, then a race for the
    // final two.
    for (std::size_t i = 1; i < 10; ++i) initial.push_back(f.Entry(i));
    auto fill =
        f.registry->JoinGroup(f.leases[0].RoomId(), {}, std::move(initial));
    for (std::size_t i = 1; i < 10; ++i)
      f.leases[i] = std::move(fill.members[i - 1]);
    f.Clear();
    rooms::RoomGroupJoinResult left, right;
    std::exception_ptr le, re;
    std::barrier gate(3);
    std::thread one([&] {
      gate.arrive_and_wait();
      try {
        left = f.Join({10, 11});
      } catch (...) {
        le = std::current_exception();
      }
    });
    std::thread two([&] {
      gate.arrive_and_wait();
      try {
        right = f.Join({12, 13});
      } catch (...) {
        re = std::current_exception();
      }
    });
    gate.arrive_and_wait();
    one.join();
    two.join();
    Check(!le && !re);
    Check((left.status == rooms::RoomJoinStatus::kJoined) !=
          (right.status == rooms::RoomJoinStatus::kJoined));
    const bool left_won = left.status == rooms::RoomJoinStatus::kJoined;
    const auto& losing = left_won ? right : left;
    Check(losing.status == rooms::RoomJoinStatus::kFull &&
          losing.members.empty());
    Check(f.registry->Find(f.leases[0].RoomId())->occupants.size() == 12);
    Sequence(f.queues[0]->Drain(), {503, 503});
    const std::size_t winner = left_won ? 10 : 12, loser = left_won ? 12 : 10;
    f.Snapshot(winner, 10, 12);
    f.Snapshot(winner + 1, 11, 12);
    Check(f.queues[loser]->Drain().empty() &&
          f.queues[loser + 1]->Drain().empty());
    Check(!f.registry->FindByCharacter(Fixture::Character(loser).character_id));
    Check(!f.registry->FindByCharacter(
        Fixture::Character(loser + 1).character_id));
  }
  // A join racing the host's start either commits the whole unready group
  // first and prevents start, or sees loading and admits nobody.
  for (int round = 0; round < 10; ++round) {
    Fixture f;
    f.Adopt(f.Create({0}).members, {0});
    f.Clear();
    std::barrier gate(3);
    rooms::RoomGroupJoinResult joined;
    rooms::RoomStartStatus started{};
    std::exception_ptr je, se;
    std::thread one([&] {
      gate.arrive_and_wait();
      try {
        joined = f.Join({1, 2});
      } catch (...) {
        je = std::current_exception();
      }
    });
    std::thread two([&] {
      gate.arrive_and_wait();
      try {
        started =
            rooms::RoomActions::Start(f.leases[0], {}, Fixture::StartRequest());
      } catch (...) {
        se = std::current_exception();
      }
    });
    gate.arrive_and_wait();
    one.join();
    two.join();
    Check(!je && !se);
    auto record = *f.registry->Find(f.leases[0].RoomId());
    if (joined.status == rooms::RoomJoinStatus::kJoined) {
      Check(started == rooms::RoomStartStatus::kMembersNotReady &&
            record.occupants.size() == 3 &&
            record.state == rooms::RoomState::kWaiting);
      Sequence(f.queues[0]->Drain(), {503, 503});
    } else {
      Check(joined.status == rooms::RoomJoinStatus::kPlaying &&
            joined.members.empty() &&
            started == rooms::RoomStartStatus::kStarted &&
            record.occupants.size() == 1 &&
            record.state == rooms::RoomState::kLoading);
      Sequence(f.queues[0]->Drain(), {585, 510});
    }
  }
  Fixture f;
  f.Adopt(f.Create({0}).members, {0});
  f.Clear();
  auto a = f.Entry(1), b = f.Entry(2);
  a.sink = [](auto) { throw std::runtime_error("Disconnected"); };
  auto joined = f.registry->JoinGroup(f.leases[0].RoomId(), {}, {a, b});
  Check(joined.members.size() == 2);
  f.Snapshot(2, 2, 3);
  Sequence(f.queues[0]->Drain(), {503, 503});
}
}  // namespace
int RunRoomAdmissionTests() {
  CreationAndRollback();
  Reservations();
  JoinAndTarget();
  GroupGame();
  CapacityAndRaces();
  std::cout << "Room admission: " << checks << " checks passed\n";
  return checks;
}
