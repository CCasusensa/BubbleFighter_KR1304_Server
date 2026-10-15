#include "features/parties/party_registry.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "features/rooms/room_packet_queue.h"

namespace bf = bubble_fighter::server;
namespace parties = bf::features::parties;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Party test line " + std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool failed = false;
  try {
    action();
  } catch (const std::exception&) {
    failed = true;
  }
  Check(failed);
}
template <class T>
void Vector(const proto::TcpPacket& packet, const T& expected) {
  Check(T::Decode(packet) == expected);
  Check(expected.Encode().message_id == packet.message_id &&
        expected.Encode().payload == packet.payload);
  for (std::size_t size = 0; size < packet.payload.size(); ++size) {
    auto short_packet = packet;
    short_packet.payload.resize(size);
    Rejects([&] { T::Decode(short_packet); });
  }
  auto extra = packet;
  extra.payload.push_back(0);
  Rejects([&] { T::Decode(extra); });
  auto wrong_id = packet;
  ++wrong_id.message_id;
  Rejects([&] { T::Decode(wrong_id); });
}
void CodecTests(const std::filesystem::path& directory) {
  std::ifstream file(directory / "party_packets.txt");
  Check(file.good());
  std::size_t rows = 0;
  for (std::string line; std::getline(file, line);) {
    if (line.empty() || line.front() == '#') continue;
    std::stringstream parser(line);
    std::array<std::string, 5> fields;
    for (auto& field : fields) std::getline(parser, field, '|');
    const auto sample = rows++ / 11;
    const auto first =
        std::array{u"Alice", u"角色", u"LongPlayer12"}.at(sample);
    const auto second = std::array{u"Bob", u"队长", u"SecondName12"}.at(sample);
    const auto value = std::array{0U, 4U, 0x12345678U}.at(sample);
    Check(std::stoul(fields[3]) == value);
    proto::TcpPacket packet{
        static_cast<std::uint16_t>(std::stoul(fields[0].substr(1))), {}};
    for (std::size_t i = 0; i < fields[4].size(); i += 2)
      packet.payload.push_back(static_cast<std::uint8_t>(
          std::stoul(fields[4].substr(i, 2), nullptr, 16)));
    const auto& kind = fields[0];
    if (kind == "c202")
      Vector(packet, proto::LeavePartyRequest{});
    else if (kind == "c302")
      Vector(packet, proto::KickPartyRequest{first});
    else if (kind == "c303")
      Vector(packet, proto::ChangePartyLeaderRequest{first});
    else if (kind == "s203")
      Vector(packet, proto::PartyLeaderLeftNotification{first, second});
    else if (kind == "s204")
      Vector(packet, proto::PartyLeaveNotification{});
    else if (kind == "s205")
      Vector(packet, proto::PartyMemberLeftNotification{first});
    else if (kind == "s206")
      Vector(packet, proto::PartyMemberJoinedNotification{first});
    else if (kind == "s302")
      Vector(packet, proto::KickPartyResponse{value, first});
    else if (kind == "s303")
      Vector(packet, proto::ChangePartyLeaderResponse{first, second, value});
    else if (kind == "s304")
      Vector(packet, proto::KickedFromPartyNotification{first});
    else if (kind == "s307")
      Vector(packet, proto::PartyKickNotification{first, second});
    else
      throw std::logic_error("Unknown party fixture");
  }
  Check(rows == 33);
  const std::u16string oversized(33, u'X'), boundary(32, u'Y');
  Rejects([&] { proto::KickPartyRequest{oversized}.Encode(); });
  Rejects([&] {
    proto::ChangePartyLeaderResponse{u"Alice", oversized, 0}.Encode();
  });
  Rejects([] { proto::KickPartyRequest::Decode({302, {255, 255, 255, 255}}); });
  Rejects([] {
    proto::KickPartyResponse::Decode({302, {0, 0, 0, 0, 33, 0, 0, 0}});
  });
  Check(proto::KickPartyRequest::Decode(
            proto::KickPartyRequest{boundary}.Encode())
            .target_name == boundary);
  const auto opaque = proto::ChangePartyLeaderResponse{u"", u"", 0xffffffff};
  Check(proto::ChangePartyLeaderResponse::Decode(opaque.Encode()) == opaque);
}

struct Fixture {
  using Character = bf::features::characters::CharacterRecord;
  struct Player {
    Character character;
    std::shared_ptr<bf::features::rooms::RoomPacketQueue> queue =
        std::make_shared<bf::features::rooms::RoomPacketQueue>();
    parties::PartyMembership membership;
  };
  bool blocked = false;
  std::shared_ptr<parties::PartyRegistry> registry;
  std::array<Player, 4> players;
  explicit Fixture(std::size_t capacity = 4) {
    registry = std::make_shared<parties::PartyRegistry>(
        capacity, [this](auto, auto) { return !blocked; });
    const std::array names{u"Alice", u"Bob", u"Carol", u"Dave"};
    for (std::size_t i = 0; i < players.size(); ++i) {
      auto& player = players[i];
      player.character = {100 + i, 200 + i, names[i], 10, 7};
      player.membership =
          registry->Enter(player.character, player.queue->Sink());
    }
  }
  auto Target(std::size_t index) {
    return *registry->Capture(players[index].character.character_name);
  }
  auto& Member(std::size_t index) { return players[index].membership; }
  auto Join(std::size_t visitor, std::size_t target) {
    return Member(visitor).JoinApproved(Target(target));
  }
  void Clear() {
    for (auto& player : players) player.queue->Clear();
  }
  std::vector<proto::TcpPacket> Take(std::size_t index,
                                     std::initializer_list<std::uint16_t> ids) {
    auto result = players[index].queue->Drain();
    Check(result.size() == ids.size());
    std::size_t i = 0;
    for (auto id : ids) Check(result[i++].message_id == id);
    return result;
  }
  void Roster(std::size_t index, std::initializer_list<std::size_t> order) {
    const auto snapshot = Member(index).Snapshot();
    Check(snapshot.state == 2 && snapshot.members.size() == order.size());
    std::size_t i = 0;
    for (auto expected : order) {
      const auto& member = snapshot.members[i++];
      Check(member.identity_value == players[expected].character.character_id &&
            member.character_name ==
                players[expected].character.character_name &&
            member.values_at32 ==
                std::array<std::uint32_t, 4>{0xffffffff, 0xffffffff, 0, 0});
    }
  }
};
void LifecycleTests() {
  Fixture f;
  for (std::size_t i = 0; i < 4; ++i) f.Roster(i, {i});
  Check(f.Join(1, 0) == parties::PartyJoinStatus::kJoined);
  for (auto i : {0U, 1U}) {
    const auto packets = f.Take(i, {201, 206});
    Check(
        proto::PartyMemberJoinedNotification::Decode(packets[1]).member_name ==
        u"Bob");
    f.Roster(i, {0, 1});
  }
  Check(f.Join(2, 1) ==
        parties::PartyJoinStatus::kJoined);  // A member may be the approved
                                             // target.
  f.Clear();
  const auto before_transfer = f.Target(0);
  f.Member(0).ChangeLeader(u"Bob");
  for (auto i : {0U, 1U, 2U}) {
    const auto packets = f.Take(i, {201, 303});
    Check(proto::ChangePartyLeaderResponse::Decode(packets[1]) ==
          proto::ChangePartyLeaderResponse{u"Bob", u"Alice", 0});
    f.Roster(i, {1, 0, 2});
  }
  Check(f.Member(3).JoinApproved(before_transfer) ==
        parties::PartyJoinStatus::kStale);
  f.Member(0).Kick(u"Carol");
  Check(proto::KickPartyResponse::Decode(f.Take(0, {302})[0]).reason == 4);
  f.Member(1).Kick(u"Bob");
  Check(proto::KickPartyResponse::Decode(f.Take(1, {302})[0]).reason == 2);
  f.Member(1).Kick(u"Offline");
  Check(proto::KickPartyResponse::Decode(f.Take(1, {302})[0]).reason == 1);
  f.Member(1).Kick(u"Dave");
  Check(proto::KickPartyResponse::Decode(f.Take(1, {302})[0]).reason == 3);
  f.Member(1).ChangeLeader(u"Bob");
  Check(proto::ChangePartyLeaderResponse::Decode(f.Take(1, {303})[0]).reason ==
        2);
  f.Member(1).ChangeLeader(u"Dave");
  Check(proto::ChangePartyLeaderResponse::Decode(f.Take(1, {303})[0]).reason ==
        1);
  f.Member(0).ChangeLeader(u"Carol");
  Check(proto::ChangePartyLeaderResponse::Decode(f.Take(0, {303})[0]).reason ==
        3);
  const auto before_kick = f.Target(2);
  f.Member(1).Kick(u"Carol");
  Check(proto::KickPartyResponse::Decode(f.Take(1, {201, 302})[1]).reason == 0);
  Check(proto::PartyKickNotification::Decode(f.Take(0, {201, 307})[1]) ==
        proto::PartyKickNotification{u"Bob", u"Carol"});
  Check(proto::KickedFromPartyNotification::Decode(f.Take(2, {201, 304})[1])
            .kicker_name == u"Bob");
  f.Roster(2, {2});
  f.Roster(1, {1, 0});
  Check(f.Member(3).JoinApproved(before_kick) ==
        parties::PartyJoinStatus::kStale);
  Check(f.Member(0).Leave());
  f.Take(0, {201, 204});
  Check(proto::PartyMemberLeftNotification::Decode(f.Take(1, {201, 205})[1])
            .member_name == u"Alice");
  Check(!f.Member(0).Leave());
  f.Take(0, {});
  f.Roster(0, {0});
  f.Roster(1, {1});
}
void DepartureTests() {
  for (bool disconnect : {false, true}) {
    Fixture f;
    Check(f.Join(1, 0) == parties::PartyJoinStatus::kJoined);
    Check(f.Join(2, 0) == parties::PartyJoinStatus::kJoined);
    f.Clear();
    const auto previous = f.Target(0);
    if (disconnect)
      f.Member(0).Release();
    else
      Check(f.Member(0).Leave());
    for (auto i : {1U, 2U}) {
      const auto packets = f.Take(i, {201, 203});
      Check(proto::PartyLeaderLeftNotification::Decode(packets[1]) ==
            proto::PartyLeaderLeftNotification{u"Alice", u"Bob"});
      f.Roster(i, {1, 2});
    }
    if (disconnect) {
      Check(!f.registry->Capture(u"Alice"));
      f.Member(0) =
          f.registry->Enter(f.players[0].character, f.players[0].queue->Sink());
      f.Take(0, {});
    } else
      f.Take(0, {201, 204});
    Check(f.Member(3).JoinApproved(previous) ==
          parties::PartyJoinStatus::kStale);
    f.Member(2).Release();
    Check(proto::PartyMemberLeftNotification::Decode(f.Take(1, {201, 205})[1])
              .member_name == u"Carol");
    f.Roster(1, {1});
    f.Member(1).Release();
    Check(!f.registry->Capture(u"Bob"));
  }
  Fixture f;
  Check(f.Join(1, 0) == parties::PartyJoinStatus::kJoined);
  Check(f.Join(3, 2) == parties::PartyJoinStatus::kJoined);
  f.Clear();
  Check(f.Join(1, 2) == parties::PartyJoinStatus::kJoined);
  f.Take(0, {201, 205});
  for (auto i : {1U, 2U, 3U}) {
    f.Take(i, {201, 206});
    f.Roster(i, {2, 3, 1});
  }
  f.Roster(0, {0});
}
void ValidationTests() {
  Rejects([] { parties::PartyRegistry registry(1); });
  Rejects([] { parties::PartyRegistry registry(9); });
  Fixture f;
  Rejects([&] {
    f.registry->Enter(f.players[0].character, f.players[1].queue->Sink());
  });
  auto bad = f.players[0].character;
  bad.character_name = u"Other";
  Rejects([&] { f.registry->Enter(bad, f.players[1].queue->Sink()); });
  bad.account_id = 999;
  Rejects([&] { f.registry->Enter(bad, f.players[1].queue->Sink()); });
  bad.character_id = 999;
  Rejects([&] { f.registry->Enter(bad, {}); });
  bad.character_id = 0;
  Rejects([&] { f.registry->Enter(bad, f.players[1].queue->Sink()); });
  parties::PartyMembership missing;
  Rejects([&] { missing.Snapshot(); });
  Rejects([&] { missing.JoinApproved(f.Target(0)); });
  Check(!f.registry->Capture(u"Absent"));
  Check(f.Join(1, 0) == parties::PartyJoinStatus::kJoined);
  Check(f.Join(1, 0) == parties::PartyJoinStatus::kAlreadyJoined);
  f.Clear();
  const auto previous = f.Target(0);
  f.Member(1).Update(f.players[1].character, true);
  Check(f.Member(2).JoinApproved(previous) == parties::PartyJoinStatus::kStale);
  Check(f.Join(2, 0) == parties::PartyJoinStatus::kBusy);
  Check(!f.Member(0).Leave());
  f.Member(0).Kick(u"Bob");
  Check(proto::KickPartyResponse::Decode(f.Take(0, {302})[0]).reason == 4);
  f.Member(0).ChangeLeader(u"Bob");
  Check(proto::ChangePartyLeaderResponse::Decode(f.Take(0, {303})[0]).reason ==
        4);
  f.Member(1).Update(f.players[1].character, false);
  f.blocked = true;
  Check(f.Join(2, 0) == parties::PartyJoinStatus::kBlocked);
  f.blocked = false;
  const auto before_rename = f.Target(0);
  auto renamed = f.players[0].character;
  renamed.character_name = u"Changed";
  proto::PartySupplementaryRecord equipment;
  equipment.key = static_cast<std::uint32_t>(renamed.character_id);
  equipment.value = 300010;
  equipment.groups[0] = {100001};
  f.Member(0).Update(renamed, false, equipment);
  Check(!f.registry->Capture(u"Alice") &&
        f.registry->Capture(u"Changed").has_value());
  Check(f.Member(3).JoinApproved(before_rename) ==
        parties::PartyJoinStatus::kStale);
  for (auto i : {0U, 1U}) {
    const auto roster =
        proto::UpdatePartyNotification::Decode(f.Take(i, {201})[0]);
    Check(roster.members[0].character_name == u"Changed" &&
          roster.supplementary_records == std::vector{equipment});
  }
  f.Member(0).Update(renamed, false, equipment);
  f.Take(0, {});
  f.Take(1, {});
  auto invalid = renamed;
  invalid.account_id = 777;
  Rejects([&] { f.Member(0).Update(invalid, false); });
  invalid = renamed;
  invalid.character_name = u"Bob";
  Rejects([&] { f.Member(0).Update(invalid, false); });
  auto wrong_equipment = equipment;
  ++wrong_equipment.key;
  Rejects([&] { f.Member(0).Update(renamed, false, wrong_equipment); });
  wrong_equipment = equipment;
  wrong_equipment.groups[0].resize(1025);
  invalid = renamed;
  invalid.character_name = u"Uncommitted";
  Rejects([&] { f.Member(0).Update(invalid, true, wrong_equipment); });
  Check(f.Member(0).Snapshot().members[0].character_name == u"Changed");
  Check(f.Member(0).Snapshot().supplementary_records == std::vector{equipment});
  Check(f.Join(2, 1) == parties::PartyJoinStatus::kJoined);
  f.Clear();
  f.Member(0).Update(renamed, true, equipment);
  f.Member(0).Release();  // Disconnect cleanup is required even while busy.
  f.Take(1, {201, 203});
  f.Take(2, {201, 203});
  f.Roster(1, {1, 2});
}
void ConcurrencyTests() {
  for (int iteration = 0; iteration < 10; ++iteration) {
    Fixture f(2);
    const auto target = f.Target(0);
    parties::PartyJoinStatus first, second;
    std::jthread one([&] { first = f.Member(1).JoinApproved(target); });
    std::jthread two([&] { second = f.Member(2).JoinApproved(target); });
    one.join();
    two.join();
    Check((first == parties::PartyJoinStatus::kJoined &&
           second == parties::PartyJoinStatus::kFull) ||
          (second == parties::PartyJoinStatus::kJoined &&
           first == parties::PartyJoinStatus::kFull));
    Check(f.Member(0).Snapshot().members.size() == 2);
    f.Roster(first == parties::PartyJoinStatus::kFull ? 1 : 2,
             {first == parties::PartyJoinStatus::kFull ? 1U : 2U});
  }
  Fixture f;
  const auto target = f.Target(0);
  parties::PartyJoinStatus first, second;
  std::jthread one([&] { first = f.Member(1).JoinApproved(target); });
  std::jthread two([&] { second = f.Member(1).JoinApproved(target); });
  one.join();
  two.join();
  Check((first == parties::PartyJoinStatus::kJoined &&
         second == parties::PartyJoinStatus::kAlreadyJoined) ||
        (second == parties::PartyJoinStatus::kJoined &&
         first == parties::PartyJoinStatus::kAlreadyJoined));
  f.Roster(0, {0, 1});
  f.Take(0, {201, 206});
  f.Take(1, {201, 206});
}
}  // namespace
int RunPartyRegistryTests(const std::filesystem::path& directory) {
  CodecTests(directory);
  LifecycleTests();
  DepartureTests();
  ValidationTests();
  ConcurrencyTests();
  std::cout << checks << " party protocol and registry checks passed\n";
  return checks;
}
