#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

#include "features/rooms/room_actions.h"
#include "features/rooms/room_creation.h"
#include "features/rooms/room_map_catalog.h"
#include "features/rooms/room_packet_queue.h"
#include "protocol/rooms/room_slot_update_notification.h"
#include "protocol/rooms/seat_state_request.h"
#include "protocol/rooms/team_change_request.h"
#include "sessions/menu_snapshot.h"

namespace bf = bubble_fighter::server;
namespace rooms = bf::features::rooms;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Room seat/team check at " +
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
  std::shared_ptr<rooms::RoomRegistry> registry = rooms::RoomRegistry::Create(
      4, {}, [] { return 12345; });
  std::array<std::shared_ptr<rooms::RoomPacketQueue>, 16> queues;
  std::array<rooms::RoomLease, 16> leases;
  Fixture() {
    for (auto& queue : queues)
      queue = std::make_shared<rooms::RoomPacketQueue>();
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
  // The hosted shape: every hosted mode plus the dialog's own room kinds.
  static rooms::RoomCreationPolicy Policy() {
    return {{1, 3, 6, 12, 40}, {0, 1, 3, 4}, 20, 6};
  }
  rooms::RoomGroupCreationOutcome Create(
      std::initializer_list<std::size_t> ids,
      proto::CreateRoomRequest request = Request()) {
    std::vector<rooms::RoomCreationInput> inputs;
    for (auto i : ids) inputs.push_back(Input(i));
    return rooms::RoomCreation::CreateGroup(*registry, Policy(),
                                            std::move(request),
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
    for (auto& queue : queues) queue->Clear();
  }
  void Empty() {
    for (auto& queue : queues) Check(queue->Drain().empty());
  }
  rooms::RoomRecord Record() {
    return *registry->Find(leases[0].RoomId());
  }
  // Every member receives the same S503 for the seat the host changed.
  void ExpectSeatUpdate(std::initializer_list<std::size_t> ids,
                        std::uint32_t seat, bool closed) {
    for (auto i : ids) {
      auto packets = queues[i]->Drain();
      Sequence(packets, {503});
      const auto update = proto::RoomSlotUpdateNotification::Decode(
          packets.front());
      Check(update.slot == seat);
      const auto expected =
          closed ? proto::SessionMember::kClosedSeatFlag : 0U;
      if (update.member.booleans_at0[0] != expected) {
        throw std::runtime_error(
            "Seat flag " + std::to_string(update.member.booleans_at0[0]) +
            " for seat " + std::to_string(update.slot) + " peer " +
            std::to_string(i) + " expected " + std::to_string(expected));
      }
      ++checks;
      Check(update.member.value_at8 == 0 && update.member.text_at12.empty());
      Check(update.records_at564.empty() && update.records_at588.empty());
    }
  }
  void ExpectTeamUpdate(std::initializer_list<std::size_t> ids,
                        std::uint32_t seat, std::uint8_t team) {
    for (auto i : ids) {
      auto packets = queues[i]->Drain();
      Sequence(packets, {503});
      const auto update = proto::RoomSlotUpdateNotification::Decode(
          packets.front());
      Check(update.slot == seat);
      Check(update.member.signed_byte_at4 == team);
    }
  }
  // S503 for an occupied seat, as a join broadcasts it to the members
  // already inside.
  void ExpectJoinUpdate(std::initializer_list<std::size_t> ids,
                        std::uint32_t seat, std::uint64_t character_id) {
    for (auto i : ids) {
      auto packets = queues[i]->Drain();
      Sequence(packets, {503});
      const auto update = proto::RoomSlotUpdateNotification::Decode(
          packets.front());
      Check(update.slot == seat);
      Check(update.member.booleans_at0[0] == 1);
      Check(update.member.value_at8 == character_id);
      Check(update.records_at564.size() == 1 &&
            update.records_at588.size() == 1);
    }
  }
  static proto::StartGameRequest Start() {
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

void SeatCodec(const std::filesystem::path& fixture) {
  std::ifstream file(fixture);
  Check(file.good());
  int rows = 0;
  for (std::string name, hex; file >> name >> hex;) {
    ++rows;
    proto::TcpPacket packet{509, {}};
    for (std::size_t offset = 0; offset < hex.size(); offset += 2) {
      packet.payload.push_back(static_cast<std::uint8_t>(
          std::stoul(hex.substr(offset, 2), nullptr, 16)));
    }
    const auto request = proto::SeatStateRequest::Decode(packet);
    Check(request.Encode().payload == packet.payload);
    if (name == "SeatOneClosed") {
      Check(request.slot == 1 && request.closed == 1);
    }
    if (name == "SeatTwoOpen") {
      Check(request.slot == 2 && request.closed == 0);
    }
    for (std::size_t length = 0; length < packet.payload.size(); ++length) {
      auto truncated = packet;
      truncated.payload.resize(length);
      Rejects([&] { proto::SeatStateRequest::Decode(truncated); });
    }
    auto wrong = packet;
    wrong.message_id = 508;
    Rejects([&] { proto::SeatStateRequest::Decode(wrong); });
    wrong = packet;
    wrong.payload.push_back(0);
    Rejects([&] { proto::SeatStateRequest::Decode(wrong); });
    // 0x00980300 and 0x0096A730 only send 0 or 1; anything else would
    // silently invert the host's click.
    wrong = packet;
    wrong.payload[4] = 2;
    Rejects([&] { proto::SeatStateRequest::Decode(wrong); });
  }
  Check(rows == 2);
}

void TeamCodec() {
  // 0x00ADEF30: the room stage's blue/red button, both markers absent.
  const proto::TcpPacket buttons{
      515, {proto::TeamChangeRequest::kAbsentMarker,
            proto::TeamChangeRequest::kAbsentMarker}};
  auto decoded = proto::TeamChangeRequest::Decode(buttons);
  Check(!decoded.value_at0 && decoded.rest.size() == 1 &&
        decoded.rest.front() == proto::TeamChangeRequest::kAbsentMarker);
  Check(decoded.Encode().payload == buttons.payload);
  // 0x004B3AC0: the clicked slot, then a structure the room ignores.
  proto::TcpPacket slot{515, {}};
  slot.payload = {proto::TeamChangeRequest::kPresentMarker, 3, 0, 0, 0,
                  proto::TeamChangeRequest::kAbsentMarker};
  const auto parsed_slot = proto::TeamChangeRequest::Decode(slot);
  Check(parsed_slot.value_at0 == 3u && parsed_slot.rest.size() == 1);
  Check(parsed_slot.Encode().payload == slot.payload);
  // 0x004A1EB0: the local slot plus a present group of two values.
  proto::TcpPacket own{515, {}};
  own.payload = {proto::TeamChangeRequest::kPresentMarker, 3, 0, 0, 0,
                 proto::TeamChangeRequest::kPresentMarker, 1, 0, 0, 0, 2, 0, 0,
                 0};
  const auto parsed_own = proto::TeamChangeRequest::Decode(own);
  Check(parsed_own.value_at0 == 3u && parsed_own.rest.size() == 9);
  Check(parsed_own.Encode().payload == own.payload);
  // A present value must carry its four bytes.
  Rejects([&] {
    proto::TeamChangeRequest::Decode(
        {515, {proto::TeamChangeRequest::kPresentMarker, 1}});
  });
  Rejects([&] { proto::TeamChangeRequest::Decode({515, {0x11}}); });
  Rejects([&] {
    proto::TeamChangeRequest::Decode({514, buttons.payload});
  });
  Rejects([&] { proto::TeamChangeRequest::Decode({515, {}}); });
}

// The create dialog's checkboxes reach the wire as Configuration.ValueAt0
// bits and the contest byte; the room accepts and echoes them.
void CreateRoomOptions() {
  Fixture f;
  auto request = Fixture::Request();
  request.settings.configuration.value_at0 =
      proto::SessionDescriptor::kRoomOptionFlags;
  request.settings.configuration.boolean_at8 = 1;
  request.settings.configuration.values_at20[0] = 1;
  request.trailing_value = 0;
  auto created = f.Create({0}, request);
  Check(created.result == proto::CreateRoomResult::kCreated);
  f.Adopt(std::move(created.members), {0});
  auto packets = f.queues[0]->Drain();
  Sequence(packets, {102, 117});
  const auto scene = proto::GotoRoomNotification::Decode(packets.back());
  Check(scene.descriptor.configuration.value_at0 ==
        proto::SessionDescriptor::kRoomOptionFlags);
  Check(scene.descriptor.configuration.boolean_at8 == 1);
  Check(scene.descriptor.configuration.values_at20[0] == 1);
  f.leases[0].Release();
  Check(f.registry->Count() == 0);

  // A kind outside the dialog's table and a bit no checkbox sets stay
  // refused, and refuse with S102 only.
  auto unknown_kind = Fixture::Request();
  unknown_kind.settings.configuration.values_at20[0] = 2;
  auto refused = f.Create({1}, unknown_kind);
  Check(refused.result == proto::CreateRoomResult::kFailed &&
        refused.members.empty());
  Sequence(refused.responses, {102});
  auto unknown_bit = Fixture::Request();
  unknown_bit.settings.configuration.value_at0 = 0x4000;
  Check(f.Create({2}, unknown_bit).result == proto::CreateRoomResult::kFailed);
  // The contest byte is one byte on the wire; the upper bytes of the field
  // are the client object's padding and never reach the room snapshot.
  auto padded = Fixture::Request();
  padded.settings.configuration.boolean_at8 = 0xABCD01;
  auto padded_room = f.Create({3}, padded);
  Check(padded_room.result == proto::CreateRoomResult::kCreated);
  f.Adopt(std::move(padded_room.members), {3});
  auto padded_packets = f.queues[3]->Drain();
  Sequence(padded_packets, {102, 117});
  Check(proto::GotoRoomNotification::Decode(padded_packets.back())
            .descriptor.configuration.boolean_at8 == 1);
  f.leases[3].Release();
  // A trailing value no dialog produces is still refused.
  auto trailing = Fixture::Request();
  trailing.trailing_value = 5;
  Check(f.Create({4}, trailing).result == proto::CreateRoomResult::kFailed);
  Check(f.registry->Count() == 0);
}

void SeatCloseAndOpen() {
  Fixture f;
  f.Adopt(f.Create({0, 1}).members, {0, 1});
  f.Clear();
  const auto room_id = f.leases[0].RoomId();
  // Only the host closes seats: 0x00980300 and 0x0096A730 case 17 both
  // test 0x009EBF60 against the local slot.
  auto refused = f.leases[1].SetSeatClosed({4, 1});
  Check(refused == rooms::RoomSeatStatus::kNotHost);
  f.Empty();
  // An occupied seat keeps its member and is never closed.
  Check(f.leases[0].SetSeatClosed({1, 1}) ==
        rooms::RoomSeatStatus::kOccupied);
  Check(f.leases[0].SetSeatClosed({12, 1}) ==
        rooms::RoomSeatStatus::kNoSuchSeat);
  f.Empty();
  Check(f.leases[0].SetSeatClosed({5, 1}) ==
        rooms::RoomSeatStatus::kChanged);
  f.ExpectSeatUpdate({0, 1}, 5, true);
  Check(f.Record().closed_seats == std::vector<std::uint32_t>{5});
  // Repeating the same state changes nothing.
  Check(f.leases[0].SetSeatClosed({5, 1}) ==
        rooms::RoomSeatStatus::kUnchanged);
  f.Empty();
  // Out-of-order closures stay ascending so the record stays canonical.
  Check(f.leases[0].SetSeatClosed({2, 1}) ==
        rooms::RoomSeatStatus::kChanged);
  f.ExpectSeatUpdate({0, 1}, 2, true);
  Check(f.Record().closed_seats == std::vector<std::uint32_t>{2, 5});
  Check(f.leases[0].SetSeatClosed({2, 0}) ==
        rooms::RoomSeatStatus::kChanged);
  f.ExpectSeatUpdate({0, 1}, 2, false);
  Check(f.Record().closed_seats == std::vector<std::uint32_t>{5});
  // A joiner takes the lowest open seat, the members see its S503 and its
  // own S117 carries the closed seat's flag.
  auto joined = f.Join({2});
  Check(joined.status == rooms::RoomJoinStatus::kJoined);
  f.leases[2] = std::move(joined.members.front());
  f.ExpectJoinUpdate({0, 1}, 2, Fixture::Character(2).character_id);
  auto packets = f.queues[2]->Drain();
  Sequence(packets, {117});
  const auto scene = proto::GotoRoomNotification::Decode(packets.front());
  Check(scene.value_at728 == 2 && scene.members.size() == 6);
  Check(scene.members[2].booleans_at0[0] == 1);
  Check(scene.members[5].booleans_at0[0] ==
        proto::SessionMember::kClosedSeatFlag);
  Check(scene.members[5].value_at8 == 0 && scene.members[3].value_at8 == 0);
  Check(scene.records_at564.size() == 3 && scene.records_at588.size() == 3);
  f.Clear();
  // Closing every remaining open seat leaves three open seats: a group of
  // four is refused as a full room and a single joiner still gets one.
  // Seat 5 is already closed from the earlier toggle.
  for (const std::uint32_t seat : {3U, 4U, 6U, 7U, 8U}) {
    Check(f.leases[0].SetSeatClosed({seat, 1}) ==
          rooms::RoomSeatStatus::kChanged);
    f.Clear();
  }
  Check(f.Record().OpenSeatCount() == 3);
  auto group = f.Join({3, 4, 5, 6});
  Check(group.status == rooms::RoomJoinStatus::kFull && group.members.empty());
  auto single = f.Join({7});
  Check(single.status == rooms::RoomJoinStatus::kJoined);
  f.leases[7] = std::move(single.members.front());
  Check(f.Record().occupants.back().slot == 9);
  f.Clear();
  // A started room cannot change its seats.
  Check(f.leases[1].SetMemberState({1, 0, 0}));
  Check(f.leases[2].SetMemberState({1, 0, 0}));
  Check(f.leases[7].SetMemberState({1, 0, 0}));
  f.Clear();
  Check(rooms::RoomActions::Start(f.leases[0], rooms::RoomPlayPolicy{},
                                  Fixture::Start()) ==
        rooms::RoomStartStatus::kStarted);
  f.Clear();
  Check(f.leases[0].SetSeatClosed({10, 1}) ==
        rooms::RoomSeatStatus::kNotWaiting);
  f.Empty();
  f.leases[0].Release();
  f.leases[1].Release();
  f.leases[2].Release();
  f.leases[7].Release();
  Check(f.registry->Find(room_id) == std::nullopt);
}

void TeamChange() {
  Fixture f;
  f.Adopt(f.Create({0, 1, 2}).members, {0, 1, 2});
  f.Clear();
  auto record = f.Record();
  Check(record.occupants[0].member.signed_byte_at4 == 1);
  Check(record.occupants[1].member.signed_byte_at4 == 2);
  // 0x00ADEF30 sends no value; the room stage draws the opposite team's
  // button, so the sender moves over.
  Check(f.leases[1].ChangeTeam({}) == rooms::RoomTeamStatus::kChanged);
  f.ExpectTeamUpdate({0, 1, 2}, 1, 1);
  Check(f.Record().occupants[1].member.signed_byte_at4 == 1);
  // An explicit team is honoured.
  proto::TeamChangeRequest explicit_team;
  explicit_team.value_at0 = 2;
  Check(f.leases[1].ChangeTeam(explicit_team) ==
        rooms::RoomTeamStatus::kChanged);
  f.ExpectTeamUpdate({0, 1, 2}, 1, 2);
  // The slot context menu names the clicked slot: adopt its occupant's team.
  proto::TeamChangeRequest by_slot;
  by_slot.value_at0 = 0;
  Check(f.leases[1].ChangeTeam(by_slot) == rooms::RoomTeamStatus::kChanged);
  f.ExpectTeamUpdate({0, 1, 2}, 1, 1);
  // A slot that holds no occupant or already matches leaves the requester
  // in the opposite team (the button's own meaning).
  proto::TeamChangeRequest empty_slot;
  empty_slot.value_at0 = 9;
  Check(f.leases[1].ChangeTeam(empty_slot) ==
        rooms::RoomTeamStatus::kChanged);
  f.ExpectTeamUpdate({0, 1, 2}, 1, 2);
  // The other two peers keep their own teams.
  Check(f.Record().occupants[0].member.signed_byte_at4 == 1);
  Check(f.Record().occupants[2].member.signed_byte_at4 == 1);
  // A playing room refuses the move and sends nothing.
  Check(f.leases[1].SetMemberState({1, 0, 0}));
  Check(f.leases[2].SetMemberState({1, 0, 0}));
  f.Clear();
  Check(rooms::RoomActions::Start(f.leases[0], rooms::RoomPlayPolicy{1},
                                  Fixture::Start()) ==
        rooms::RoomStartStatus::kStarted);
  f.Clear();
  Check(f.leases[1].ChangeTeam({}) == rooms::RoomTeamStatus::kNotWaiting);
  f.Empty();
}
}  // namespace

int RunRoomSeatTeamTests(const std::filesystem::path& fixture) {
  SeatCodec(fixture);
  TeamCodec();
  CreateRoomOptions();
  SeatCloseAndOpen();
  TeamChange();
  std::cout << "Room seat/team: " << checks << " checks passed\n";
  return checks;
}
