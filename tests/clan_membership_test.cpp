#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <span>
#include <stdexcept>

#include "protocol/clans/clan_membership_messages.h"

namespace proto = bubble_fighter::server::protocol;
namespace {
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Clan membership line " +
                             std::to_string(at.line()));
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
std::vector<std::uint8_t> Hex(const std::string& text) {
  if (text == "-") return {};
  Check(text.size() % 2 == 0);
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i < text.size(); i += 2)
    bytes.push_back(
        static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
  return bytes;
}
struct Fixture {
  proto::ClanPacket packet;
  std::vector<std::uint8_t> frame;
};
std::map<std::string, Fixture> fixtures;
template <class Message>
void Verify(const std::string& name, const Message& expected) {
  const auto& fixture = fixtures.at(name);
  Check(expected.Encode() == fixture.packet);
  Check(Message::Decode(fixture.packet) == expected);
  Check(proto::ClanPacketCodec::Encode(fixture.packet) == fixture.frame);
  auto bad = fixture.packet;
  ++bad.message_id;
  Rejects([&] { Message::Decode(bad); });
  bad = fixture.packet;
  bad.payload.push_back(0);
  Rejects([&] { Message::Decode(bad); });
  bad.payload.resize(proto::ClanPacketCodec::kMaxPayloadSize + 1);
  Rejects([&] { Message::Decode(bad); });
  for (std::size_t size = 0; size < fixture.packet.payload.size(); ++size) {
    bad = fixture.packet;
    bad.payload.resize(size);
    Rejects([&] { Message::Decode(bad); });
  }
  for (std::size_t split = 0; split <= fixture.frame.size(); ++split) {
    proto::ClanStreamDecoder decoder;
    const std::span<const std::uint8_t> frame = fixture.frame;
    auto packets = decoder.Feed(frame.first(split));
    const auto tail = decoder.Feed(frame.subspan(split));
    packets.insert(packets.end(), tail.begin(), tail.end());
    decoder.Finish();
    Check(packets == std::vector<proto::ClanPacket>{fixture.packet});
  }
}
void NativeFixtures() {
  struct Values {
    const char* label;
    std::uint32_t result, target, old_status, new_status;
    std::u16string clan, name;
  };
  const std::array<Values, 3> values{{
      {"zero", 0, 0, 0, 0, u"", u""},
      {"normal", 1, 71, 1, 100, u"ClanName", u"Fighter"},
      {"upper", 0xffffffff, 0x87654321, 0xffffffec, 0x12345678, u"水泡🚀",
       u"玩家B"},
  }};
  for (const auto& v : values) {
    const auto prefix = std::string(v.label) + "_";
    Verify(prefix + "1032", proto::ClanJoinRequest{v.target, v.clan});
    Verify(prefix + "1033",
           proto::ClanAutoJoinRequest{v.target, v.clan, v.name});
    Verify(prefix + "1034",
           proto::ClanJoinResponse{v.result, v.old_status, v.target, v.clan});
    Verify(prefix + "1037",
           proto::ClanLeaveNotification{v.clan, v.target, v.name, v.old_status,
                                        v.new_status});
    Verify(prefix + "1039", proto::ClanMemberChangeRequest{
                                v.target, v.name, v.old_status, v.new_status});
    Verify(prefix + "1040", proto::ClanMemberChangeResponse{
                                v.result, v.name, v.new_status, v.old_status});
    Verify(prefix + "1041",
           proto::ClanMemberChangedNotification{v.clan, v.target, v.name,
                                                v.new_status, v.old_status});
    Verify(prefix + "1063", proto::ClanCancelJoinRequest{v.target});
    Verify(prefix + "1064", proto::ClanCancelJoinResponse{v.result, v.target});
    Verify(prefix + "1065",
           proto::ClanJoinCancelledNotification{v.result, v.target, v.name});
  }
  Verify("empty_1035", proto::ClanApplicationChangedNotification{});
  Verify("empty_1036", proto::ClanLeaveRequest{});
  // These otherwise-identical notifications have opposite wire status order.
  auto leave = fixtures.at("normal_1037").packet;
  leave.message_id = 1041;
  const auto changed = proto::ClanMemberChangedNotification::Decode(leave);
  Check(changed.new_status == 1 && changed.old_status == 100);
}
template <class Message, std::size_t N>
void TextBoundaries(std::size_t scalar_bytes,
                    const std::array<std::u16string Message::*, N>& fields) {
  for (const auto field : fields) {
    Message message;
    message.*field = std::u16string{u'A', u'\0', u'B'};
    Rejects([&] { message.Encode(); });
    (message.*field).assign(65531, u'A');
    Rejects([&] { message.Encode(); });
    const auto max_units = (65531 - scalar_bytes - 2 * N) / 2;
    (message.*field).assign(max_units, u'A');
    const auto packet = message.Encode();
    Check(packet.payload.size() == 65530);
    Check(Message::Decode(packet) == message);
    (message.*field).push_back(u'B');
    Rejects([&] { message.Encode(); });
    // No Unicode normalization in codecs, including unpaired surrogate units.
    message.*field = std::u16string{char16_t{0xd800}, u'A', char16_t{0xdfff}};
    Check(Message::Decode(message.Encode()) == message);
    auto unterminated = Message{}.Encode();
    std::fill(unterminated.payload.begin(), unterminated.payload.end(),
              std::uint8_t{255});
    Rejects([&] { Message::Decode(unterminated); });
  }
  if constexpr (N > 1) {
    Message message;
    const auto max_units = (65531 - scalar_bytes - 2 * N) / 2;
    (message.*fields[0]).assign(max_units / 2, u'A');
    (message.*fields[1]).assign(max_units - max_units / 2, u'B');
    Check(message.Encode().payload.size() == 65530);
    Check(Message::Decode(message.Encode()) == message);
    (message.*fields[1]).push_back(u'C');
    Rejects([&] { message.Encode(); });
  }
}
void Boundaries() {
  TextBoundaries<proto::ClanJoinRequest>(
      4, std::array{&proto::ClanJoinRequest::clan_name});
  TextBoundaries<proto::ClanAutoJoinRequest>(
      4, std::array{&proto::ClanAutoJoinRequest::clan_name,
                    &proto::ClanAutoJoinRequest::character_name});
  TextBoundaries<proto::ClanJoinResponse>(
      12, std::array{&proto::ClanJoinResponse::clan_name});
  TextBoundaries<proto::ClanLeaveNotification>(
      12, std::array{&proto::ClanLeaveNotification::clan_name,
                     &proto::ClanLeaveNotification::character_name});
  TextBoundaries<proto::ClanMemberChangeRequest>(
      12, std::array{&proto::ClanMemberChangeRequest::character_name});
  TextBoundaries<proto::ClanMemberChangeResponse>(
      12, std::array{&proto::ClanMemberChangeResponse::character_name});
  TextBoundaries<proto::ClanMemberChangedNotification>(
      12, std::array{&proto::ClanMemberChangedNotification::clan_name,
                     &proto::ClanMemberChangedNotification::character_name});
  TextBoundaries<proto::ClanJoinCancelledNotification>(
      8, std::array{&proto::ClanJoinCancelledNotification::character_name});
}
}  // namespace

int RunClanMembershipTests(const std::filesystem::path& directory) {
  std::ifstream input(directory / "clan_membership_packets.txt");
  Check(input.good());
  std::string label, payload, frame;
  std::uint16_t id = 0;
  while (input >> label >> id >> payload >> frame)
    Check(fixtures.emplace(label, Fixture{{id, Hex(payload)}, Hex(frame)})
              .second);
  Check(input.eof());
  Check(fixtures.size() == 32);
  NativeFixtures();
  Boundaries();
  std::cout << "Clan membership: " << checks << " checks passed\n";
  return checks;
}
