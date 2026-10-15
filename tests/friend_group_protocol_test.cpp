#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "protocol/codec/packet_writer.h"
#include "protocol/social/companion_friend_group_messages.h"

namespace {
namespace p = bubble_fighter::server::protocol;
using Action = p::CompanionFriendGroupAction;
using Status = p::CompanionFriendGroupStatus;
using Request = p::CompanionFriendGroupRequest;
using Snapshot = p::CompanionFriendGroupsNotification;
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value) {
    throw std::runtime_error("Friend group protocol line " +
                             std::to_string(at.line()));
  }
}
template <class F>
void Rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected);
}
template <class Message>
void Framing(const p::TcpPacket& packet) {
  Check(Message::Decode(packet).Encode().payload == packet.payload);
  for (std::size_t size = 0; size < packet.payload.size(); ++size) {
    auto broken = packet;
    broken.payload.resize(size);
    Rejects([&] { (void)Message::Decode(broken); });
  }
  auto broken = packet;
  broken.payload.push_back(0);
  Rejects([&] { (void)Message::Decode(broken); });
  broken = packet;
  broken.message_id = 65010;
  Rejects([&] { (void)Message::Decode(broken); });
  broken = packet;
  broken.payload[0] = 2;
  Rejects([&] { (void)Message::Decode(broken); });
}
p::TcpPacket RawRequest(Action action, std::uint32_t group, std::uint32_t peer,
                        std::uint32_t source, std::u16string_view name) {
  p::PacketWriter writer;
  writer.WriteUInt32(1);
  writer.WriteUInt8(static_cast<std::uint8_t>(action));
  writer.WriteUInt32(group);
  writer.WriteUInt32(peer);
  writer.WriteUInt32(source);
  writer.WriteUtf16(name);
  return {Request::kMessageId, writer.TakeBytes()};
}
}  // namespace

int RunFriendGroupProtocolTests(const std::filesystem::path& directory) {
  checks = 0;
  const std::array<Request, 8> requests{{
      {},
      {Action::kCreate, 0, 0, 0, u"친구"},
      {Action::kRename, 7, 0, 0, u"队友\U0001F600"},
      {Action::kRemove, 7, 0, 0, {}},
      {Action::kMove, 7, 42, 0, {}},
      {Action::kMove, 9, 42, 7, {}},
      {Action::kMove, 0, 42, 9, {}},
      {Action::kCreate, 0, 0, 0, std::u16string(31, u'G')},
  }};
  const std::array<Snapshot, 4> snapshots{{
      {},
      {Status::kOk,
       Action::kMove,
       {{7, u"친구"}, {9, u"队友\U0001F600"}},
       {{42, 7}, {43, 9}}},
      {Status::kNotEmpty, Action::kRemove, {{7, u"친구"}}, {{42, 7}}},
      {Status::kUnavailable, Action::kList, {}, {}},
  }};
  std::ifstream file(directory / "friend_group_packets.txt");
  Check(file.good());
  std::string label, hex;
  std::uint16_t id = 0;
  std::size_t request_index = 0, snapshot_index = 0;
  while (file >> label >> id >> hex) {
    p::TcpPacket packet{id, {}};
    Check(hex.size() % 2 == 0);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      packet.payload.push_back(
          static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    if (id == Request::kMessageId) {
      Check(request_index < requests.size());
      const auto& expected = requests.at(request_index++);
      Check(expected.Encode().payload == packet.payload);
      const auto actual = Request::Decode(packet);
      Check(actual.action == expected.action &&
            actual.group_id == expected.group_id &&
            actual.friend_id == expected.friend_id &&
            actual.expected_group_id == expected.expected_group_id &&
            actual.name == expected.name);
      Framing<Request>(packet);
    } else {
      Check(id == Snapshot::kMessageId && snapshot_index < snapshots.size());
      const auto& expected = snapshots.at(snapshot_index++);
      Check(expected.Encode().payload == packet.payload);
      const auto actual = Snapshot::Decode(packet);
      Check(actual.action == expected.action &&
            actual.status == expected.status &&
            actual.groups == expected.groups &&
            actual.assignments == expected.assignments);
      Framing<Snapshot>(packet);
    }
  }
  Check(request_index == requests.size() && snapshot_index == snapshots.size());

  // Invalid action-specific fields must not acquire meaning in the business
  // layer.
  const std::array<Request, 16> invalid{{
      {Action::kList, 1, 0, 0, {}},
      {Action::kList, 0, 1, 0, {}},
      {Action::kList, 0, 0, 1, {}},
      {Action::kList, 0, 0, 0, u"x"},
      {Action::kCreate, 7, 0, 0, u"x"},
      {Action::kCreate, 0, 42, 0, u"x"},
      {Action::kCreate, 0, 0, 7, u"x"},
      {Action::kRename, 0, 0, 0, u"x"},
      {Action::kRename, 7, 42, 0, u"x"},
      {Action::kRename, 7, 0, 9, u"x"},
      {Action::kRemove, 0, 0, 0, {}},
      {Action::kRemove, 7, 42, 0, {}},
      {Action::kRemove, 7, 0, 9, {}},
      {Action::kRemove, 7, 0, 0, u"x"},
      {Action::kMove, 7, 0, 0, {}},
      {Action::kMove, 7, 42, 7, {}},
  }};
  for (const auto& bad : invalid) {
    Rejects([&] { (void)bad.Encode(); });
    Rejects([&] {
      (void)Request::Decode(RawRequest(bad.action, bad.group_id, bad.friend_id,
                                       bad.expected_group_id, bad.name));
    });
  }
  for (const auto& bad : std::array<Request, 2>{{
           {Action::kMove, 7, 42, 0, u"x"},
           {static_cast<Action>(255), 0, 0, 0, {}},
       }}) {
    Rejects([&] { (void)bad.Encode(); });
    Rejects([&] {
      (void)Request::Decode(RawRequest(bad.action, bad.group_id, bad.friend_id,
                                       bad.expected_group_id, bad.name));
    });
  }
  const std::vector<std::u16string> invalid_names{
      {},
      std::u16string(32, u'G'),
      u" ",
      u"　",
      u" 　 ",
      std::u16string(u"A\0B", 3),
      u"A\nB",
      u"A\tB",
      std::u16string(1, 0x7F),
      std::u16string(1, 0x85),
      std::u16string(1, 0xD800),
      std::u16string(1, 0xDC00),
      {0xD800, u'A'},
      {0xD800, 0xD800},
      {0xDC00, 0xD800},
      std::u16string(30, u'G') + u"\U0001F600"};
  for (const auto& name : invalid_names) {
    Check(!p::ValidCompanionFriendGroupName(name));
    Rejects([&] { (void)Request{Action::kCreate, 0, 0, 0, name}.Encode(); });
    Rejects([&] {
      (void)Request::Decode(RawRequest(Action::kCreate, 0, 0, 0, name));
    });
    Rejects([&] {
      (void)Snapshot{Status::kOk, Action::kList, {{7, name}}, {}}.Encode();
    });
  }
  const auto exact = std::u16string(29, u'G') + u"\U0001F600";
  Check(p::ValidCompanionFriendGroupName(exact));
  Check(
      Request::Decode(Request{Action::kCreate, 0, 0, 0, exact}.Encode()).name ==
      exact);
  Check(p::ValidCompanionFriendGroupName(u" Team "));

  Snapshot maximum;
  for (std::uint32_t i = 1; i <= Snapshot::kMaximumGroups; ++i) {
    maximum.groups.push_back(
        {i, std::u16string(1, static_cast<char16_t>(0x4E00 + i))});
  }
  for (std::uint32_t i = 1; i <= Snapshot::kMaximumAssignments; ++i) {
    maximum.assignments.push_back({i, i % 32 + 1});
  }
  const auto decoded = Snapshot::Decode(maximum.Encode());
  Check(decoded.groups == maximum.groups &&
        decoded.assignments == maximum.assignments);
  auto bad = maximum;
  bad.groups.push_back({33, u"excess"});
  Rejects([&] { (void)bad.Encode(); });
  bad = maximum;
  bad.assignments.push_back({201, 1});
  Rejects([&] { (void)bad.Encode(); });
  for (int variant = 0; variant < 9; ++variant) {
    bad = snapshots[1];
    switch (variant) {
      case 0:
        bad.groups[0].group_id = 0;
        break;
      case 1:
        bad.groups[1].group_id = 7;
        break;
      case 2:
        bad.groups[1].name = bad.groups[0].name;
        break;
      case 3:
        bad.assignments[0].friend_id = 0;
        break;
      case 4:
        bad.assignments[1].friend_id = 42;
        break;
      case 5:
        bad.assignments[0].group_id = 0;
        break;
      case 6:
        bad.assignments[0].group_id = 8;
        break;
      case 7:
        bad.status = static_cast<Status>(9);
        break;
      case 8:
        bad.action = static_cast<Action>(5);
        break;
    }
    Rejects([&] { (void)bad.Encode(); });
  }
  // Forged counts/identities are checked on decode, before a caller can use
  // them.
  for (std::uint32_t count : {33U, 0xFFFFFFFFU}) {
    p::PacketWriter writer;
    writer.WriteUInt32(1);
    writer.WriteUInt32(0);
    writer.WriteUInt8(0);
    writer.WriteUInt32(count);
    Rejects([&] { (void)Snapshot::Decode({65013, writer.TakeBytes()}); });
  }
  for (std::uint32_t count : {201U, 0xFFFFFFFFU}) {
    p::PacketWriter writer;
    writer.WriteUInt32(1);
    writer.WriteUInt32(0);
    writer.WriteUInt8(0);
    writer.WriteUInt32(0);
    writer.WriteUInt32(count);
    Rejects([&] { (void)Snapshot::Decode({65013, writer.TakeBytes()}); });
  }
  auto forged = requests[1].Encode();
  for (std::size_t i = 17; i < 21; ++i) forged.payload[i] = 0xFF;
  Rejects([&] { (void)Request::Decode(forged); });
  forged = snapshots[1].Encode();
  // First assignment references an absent group. Fixed layout comes from
  // fixture.
  const auto group_offset = forged.payload.size() - 12;
  forged.payload[group_offset] = 8;
  Rejects([&] { (void)Snapshot::Decode(forged); });
  forged = snapshots[1].Encode();
  forged.payload[forged.payload.size() - 8] = 42;
  Rejects([&] { (void)Snapshot::Decode(forged); });
  forged = snapshots[1].Encode();
  forged.payload[4] = 9;
  Rejects([&] { (void)Snapshot::Decode(forged); });
  forged = snapshots[1].Encode();
  forged.payload[8] = 5;
  Rejects([&] { (void)Snapshot::Decode(forged); });
  std::cout << "Friend group protocol: " << checks << " checks passed.\n";
  return checks;
}
