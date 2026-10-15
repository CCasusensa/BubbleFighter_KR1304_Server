#include <array>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "features/lobby/plaza_registry.h"
#include "protocol/rooms/room_member_left_notification.h"
#include "protocol/rooms/room_slot_update_notification.h"
#include "sessions/menu_snapshot.h"

namespace bf = bubble_fighter::server;
namespace lobby = bf::features::lobby;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Plaza admission line " +
                             std::to_string(at.line()));
}
template <class Action>
void Rejects(Action action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected);
}
void Sequence(const std::vector<proto::TcpPacket>& packets,
              std::initializer_list<std::uint16_t> ids) {
  Check(packets.size() == ids.size());
  auto at = ids.begin();
  for (const auto& packet : packets) Check(packet.message_id == *at++);
}
struct Fixture {
  std::shared_ptr<lobby::PlazaRegistry> registry =
      std::make_shared<lobby::PlazaRegistry>();
  std::array<std::shared_ptr<bf::features::rooms::RoomPacketQueue>, 24> queues;
  std::array<lobby::PlazaLease, 24> leases;
  Fixture() {
    for (auto& queue : queues)
      queue = std::make_shared<bf::features::rooms::RoomPacketQueue>();
  }
  static bf::features::characters::CharacterRecord Character(std::size_t i) {
    return {100 + i, 200 + i,
            u"Peer" + std::u16string(1, static_cast<char16_t>(u'A' + i)), 10,
            7};
  }
  static bf::sessions::MenuInitializationData Data(std::size_t i) {
    const auto character = Character(i);
    return bf::sessions::CreateMenuSnapshot(
        character, {character.account_id, character.character_id, {}}, 1000);
  }
  lobby::PlazaEntry Entry(std::size_t i) {
    auto data = Data(i);
    return {{std::move(data.member), Character(i), std::move(data.equipment)},
            Character(i).account_id,
            queues.at(i)->Sink(),
            leases.at(i).Token()};
  }
  lobby::PlazaTarget Target(std::size_t i) {
    return *registry->Capture(Character(i).account_id);
  }
  void Clear() {
    for (auto& queue : queues) queue->Clear();
  }
  void Empty() {
    for (auto& queue : queues) Check(queue->Drain().empty());
  }
  lobby::PlazaAdmission Group(std::initializer_list<std::size_t> members,
                              std::optional<lobby::PlazaTarget> target = {}) {
    std::vector<lobby::PlazaEntry> entries;
    for (auto i : members) entries.push_back(Entry(i));
    return registry->EnterGroup(Data(*members.begin()).scene,
                                std::move(entries), target);
  }
  void Adopt(lobby::PlazaAdmission result,
             std::initializer_list<std::size_t> members) {
    Check(result.status == lobby::PlazaAdmissionStatus::kEntered &&
          result.members.size() == members.size());
    std::size_t index = 0;
    for (auto i : members) leases.at(i) = std::move(result.members.at(index++));
  }
  void Enter(std::size_t i) {
    auto entry = Entry(i);
    leases.at(i) = registry->Enter(Data(i).scene, std::move(entry.player),
                                   entry.account, std::move(entry.sink));
  }
  void Fill(std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) Enter(i);
    Clear();
  }
  proto::GotoRoomNotification Own(std::size_t i, std::size_t count,
                                  bool movement = false) {
    const auto packets = queues.at(i)->Drain();
    if (movement)
      Sequence(packets, {117, 514});
    else
      Sequence(packets, {117});
    auto snapshot = proto::GotoRoomNotification::Decode(packets.at(0));
    Check(snapshot.descriptor.session_type == 3 &&
          snapshot.descriptor.shorts_at32[0] == count);
    Check(snapshot.members.at(snapshot.value_at728).value_at8 ==
          Character(i).character_id);
    Check(snapshot.members.at(snapshot.value_at728).text_at12 ==
          Character(i).character_name);
    Check(snapshot.records_at564.size() == count &&
          snapshot.records_at588.size() == count);
    return snapshot;
  }
};
void GroupPlacement() {
  Fixture f;
  f.Fill(15);
  auto joined = f.Group({15, 16});
  f.Adopt(std::move(joined), {15, 16});
  Check(f.Target(0).scene == 1 && f.Target(15).scene == 2 &&
        f.Target(16).scene == 2);
  f.Own(15, 2);
  f.Own(16, 2);
  for (std::size_t i = 0; i < 15; ++i) Check(f.queues[i]->Drain().empty());
  const auto target = f.Target(0);
  auto full = f.Group({17, 18}, target);
  Check(full.status == lobby::PlazaAdmissionStatus::kFull &&
        full.members.empty());
  Check(!f.registry->Capture(Fixture::Character(17).account_id));
  Check(!f.registry->Capture(Fixture::Character(18).account_id));
  f.Empty();
  f.Adopt(f.Group({17}, target), {17});
  f.Own(17, 16);
  for (std::size_t i = 0; i < 15; ++i)
    Sequence(f.queues[i]->Drain(), {503, 501});
  Check(f.Target(17).scene == 1);
}
void TransferAndStaleLease() {
  Fixture f;
  f.Fill(14);
  f.Adopt(f.Group({14, 15, 16}), {14, 15, 16});
  f.Clear();
  proto::PlazaMovementRequest movement;
  movement.sequence = 77;
  f.leases[0].Move(movement);
  f.Clear();
  const auto old = f.Target(5);
  auto moved = f.Group({5, 14, 15}, f.Target(0));
  Check(moved.status == lobby::PlazaAdmissionStatus::kEntered);
  for (auto i : {5U, 14U, 15U}) f.Own(i, 16, true);
  Sequence(f.queues[16]->Drain(), {511, 511, 501});
  for (std::size_t i = 0; i < 14; ++i) {
    if (i == 5) continue;
    const auto packets = f.queues[i]->Drain();
    Sequence(packets, {511, 503, 503, 503, 501});
    Check(proto::RoomMemberLeftNotification::Decode(packets[0]).slot == 5);
    Check(proto::RoomSlotUpdateNotification::Decode(packets[1])
              .member.value_at8 == 105);
  }
  Check(f.Target(5).token != old.token && f.Target(14).scene == 1 &&
        f.Target(16).scene == 2);
  f.leases[5].Move(movement);
  f.leases[5].Chat({0, u"stale", {}, {}});
  f.leases[5].Refresh({}, true);
  f.leases[5].Release();
  Check(f.registry->Capture(Fixture::Character(5).account_id).has_value());
  f.Empty();
  f.Adopt(std::move(moved), {5, 14, 15});
  f.Empty();
  f.leases[5].Move(movement);
  for (std::size_t i = 0; i < 16; ++i) {
    if (i != 5) Sequence(f.queues[i]->Drain(), {514});
  }
  Check(f.queues[16]->Drain().empty());
  f.leases[5].Release();
  Check(!f.registry->Capture(Fixture::Character(5).account_id));
}
void RollbackAndReuse() {
  Fixture f;
  f.Fill(3);
  const auto old = f.Target(0);
  auto entries = std::vector{f.Entry(1), f.Entry(3)};
  entries.back().player.equipment->groups[0].resize(1025);
  Rejects(
      [&] { f.registry->EnterGroup(Fixture::Data(1).scene, entries, old); });
  Check(f.Target(0).token == old.token && f.Target(1).scene == old.scene);
  Check(!f.registry->Capture(Fixture::Character(3).account_id));
  f.Empty();
  entries = {f.Entry(1), f.Entry(3)};
  ++entries.front().source_token;
  Check(f.registry->EnterGroup(Fixture::Data(1).scene, entries, old).status ==
        lobby::PlazaAdmissionStatus::kStale);
  f.Empty();
  f.leases[0].Release();
  f.Enter(0);
  f.Clear();
  Check(f.Target(0).token != old.token);
  Check(f.Group({3}, old).status == lobby::PlazaAdmissionStatus::kStale);
  f.Empty();
  f.Adopt(f.Group({1, 2}, f.Target(0)), {1, 2});
  f.Own(1, 3);
  f.Own(2, 3);
  Sequence(f.queues[0]->Drain(), {511, 511, 503, 503, 501});
  f.leases[0].Release();
  f.leases[1].Release();
  f.leases[2].Release();
  f.Clear();
  f.Enter(0);
  f.Clear();
  Check(f.Target(0).scene == old.scene && f.Target(0).token != old.token);
  Check(f.Group({3}, old).status == lobby::PlazaAdmissionStatus::kStale);
  f.Empty();
}
void InputAndDelivery() {
  Fixture f;
  Rejects([&] { f.registry->EnterGroup(Fixture::Data(0).scene, {}); });
  Rejects([&] {
    f.registry->EnterGroup(Fixture::Data(0).scene, {f.Entry(0), f.Entry(0)});
  });
  auto bad = f.Entry(0);
  bad.player.equipment->key = 9;
  Rejects([&] { f.registry->EnterGroup(Fixture::Data(0).scene, {bad}); });
  std::vector<lobby::PlazaEntry> many;
  for (std::size_t i = 0; i < 17; ++i) many.push_back(f.Entry(i));
  Rejects([&] { f.registry->EnterGroup(Fixture::Data(0).scene, many); });
  auto first = f.Entry(0);
  first.sink = [](auto) {
    throw std::runtime_error("Closed synthetic recipient");
  };
  auto result =
      f.registry->EnterGroup(Fixture::Data(0).scene, {first, f.Entry(1)});
  f.Adopt(std::move(result), {0, 1});
  f.Own(1, 2);
  Check(f.Target(0).scene == f.Target(1).scene);
  Rejects([&] { f.Enter(0); });
  Check(f.Target(0).token == f.leases[0].Token());
  f.Empty();
}
void ConcurrentAdmission() {
  for (int iteration = 0; iteration < 10; ++iteration) {
    Fixture f;
    f.Fill(14);
    const auto target = f.Target(0);
    lobby::PlazaAdmission one, two;
    std::jthread a([&] { one = f.Group({14, 15}, target); });
    std::jthread b([&] { two = f.Group({16, 17}, target); });
    a.join();
    b.join();
    Check((one.status == lobby::PlazaAdmissionStatus::kEntered &&
           two.status == lobby::PlazaAdmissionStatus::kFull) ||
          (two.status == lobby::PlazaAdmissionStatus::kEntered &&
           one.status == lobby::PlazaAdmissionStatus::kFull));
    const auto winner =
        one.status == lobby::PlazaAdmissionStatus::kEntered ? 14U : 16U;
    f.Own(winner, 16);
    f.Own(winner + 1, 16);
    Check(
        f.registry->Capture(Fixture::Character(winner).account_id).has_value());
    const auto loser = winner == 14 ? 16U : 14U;
    Check(!f.registry->Capture(Fixture::Character(loser).account_id));
    Check(!f.registry->Capture(Fixture::Character(loser + 1).account_id));
  }
  Fixture f;
  f.Fill(3);
  const auto target = f.Target(0);
  // Identical prepared source tokens cannot be committed twice.
  auto entry = f.Entry(1);
  lobby::PlazaAdmission one, two;
  std::jthread a([&] {
    one = f.registry->EnterGroup(Fixture::Data(0).scene, {entry}, target);
  });
  std::jthread b([&] {
    two = f.registry->EnterGroup(Fixture::Data(0).scene, {entry}, target);
  });
  a.join();
  b.join();
  Check((one.status == lobby::PlazaAdmissionStatus::kEntered &&
         two.status == lobby::PlazaAdmissionStatus::kStale) ||
        (two.status == lobby::PlazaAdmissionStatus::kEntered &&
         one.status == lobby::PlazaAdmissionStatus::kStale));
  f.Own(1, 3);
}
}  // namespace
int RunPlazaAdmissionTests() {
  GroupPlacement();
  TransferAndStaleLease();
  RollbackAndReuse();
  InputAndDelivery();
  ConcurrentAdmission();
  std::cout << checks << " atomic plaza admission checks passed\n";
  return checks;
}
