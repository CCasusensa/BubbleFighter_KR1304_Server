#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <span>
#include <stdexcept>

#include "protocol/clans/clan_master_transfer_messages.h"

namespace proto = bubble_fighter::server::protocol;
namespace {
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Clan master-transfer codec line " +
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
    std::uint8_t flag;
    std::uint32_t opaque, former, target;
    std::u16string name, former_name;
  };
  const std::array<Values, 3> values{{
      {"zero", 0, 0, 0, 0, u"", u""},
      {"normal", 1, 0x12345678, 42, 73, u"Successor", u"Master"},
      {"upper", 255, 0xffffffff, 0x87654321, 0xfedcba98, u"玩家🚀",
       u"水泡한국"},
  }};
  for (const auto& v : values) {
    const auto prefix = std::string(v.label) + "_";
    Verify(prefix + "1054",
           proto::ClanMasterTransferCommitRequest{v.target, v.name});
    Verify(prefix + "1056",
           proto::ClanMasterTransferredNotification{
               v.opaque, v.former, v.former_name, v.target, v.name});
    Verify(prefix + "1057", proto::ClanMasterTransferRequest{v.target});
    Verify(prefix + "1058",
           proto::ClanMasterTransferResponse{v.flag, v.opaque});
    Verify(prefix + "1059",
           proto::ClanMasterTransferOfferNotification{v.former_name, v.opaque});
    Verify(prefix + "1060",
           proto::ClanMasterTransferAnswerRequest{
               static_cast<std::uint8_t>(v.flag == 1), v.opaque});
    Verify(prefix + "1061", proto::ClanMasterTransferAnswerResponse{v.flag});
    Verify(prefix + "1062", proto::ClanMasterTransferDecisionNotification{
                                v.flag, v.target, v.name});
  }
}
void Boundaries() {
  // Byte-prefixed S1062 can fill all 65,531 bytes, including its terminator.
  proto::ClanMasterTransferDecisionNotification decision{
      1, 42, std::u16string(32762, u'X')};
  Check(decision.Encode().payload.size() ==
        proto::ClanPacketCodec::kMaxPayloadSize);
  Check(proto::ClanMasterTransferDecisionNotification::Decode(
            decision.Encode()) == decision);
  decision.character_name.push_back(u'X');
  Rejects([&] { decision.Encode(); });
  proto::ClanMasterTransferCommitRequest commit{42,
                                                std::u16string(32762, u'X')};
  Check(commit.Encode().payload.size() == 65530);
  commit.character_name.push_back(u'X');
  Rejects([&] { commit.Encode(); });
  proto::ClanMasterTransferOfferNotification offer{std::u16string(32762, u'X'),
                                                   7};
  Check(offer.Encode().payload.size() == 65530);
  offer.requester_name.push_back(u'X');
  Rejects([&] { offer.Encode(); });
  // Both names share the same frame budget; individually legal is insufficient.
  proto::ClanMasterTransferredNotification changed{
      0, 42, std::u16string(16000, u'A'), 73, std::u16string(16757, u'B')};
  Check(changed.Encode().payload.size() == 65530);
  Check(proto::ClanMasterTransferredNotification::Decode(changed.Encode()) ==
        changed);
  changed.new_name.push_back(u'B');
  Rejects([&] { changed.Encode(); });
  const std::u16string embedded{u'A', u'\0', u'B'};
  Rejects(
      [&] { proto::ClanMasterTransferCommitRequest{1, embedded}.Encode(); });
  Rejects([&] {
    proto::ClanMasterTransferOfferNotification{embedded, 1}.Encode();
  });
  Rejects([&] {
    proto::ClanMasterTransferDecisionNotification{1, 1, embedded}.Encode();
  });
  Rejects([&] {
    proto::ClanMasterTransferredNotification{0, 1, embedded, 2, u"B"}.Encode();
  });
  Rejects([&] {
    proto::ClanMasterTransferredNotification{0, 1, u"A", 2, embedded}.Encode();
  });
  proto::ClanMasterTransferDecisionNotification raw{
      255, 0xffffffff, std::u16string{static_cast<char16_t>(0xd800)}};
  Check(proto::ClanMasterTransferDecisionNotification::Decode(raw.Encode()) ==
        raw);
  auto missing = raw.Encode();
  missing.payload.resize(missing.payload.size() - 2);
  Rejects(
      [&] { proto::ClanMasterTransferDecisionNotification::Decode(missing); });
}
}  // namespace

int RunClanMasterTransferCodecTests(const std::filesystem::path& directory) {
  std::ifstream input(directory / "clan_master_transfer_packets.txt");
  Check(input.good());
  std::string label, plain, wire;
  std::uint16_t mid = 0;
  while (input >> label >> mid >> plain >> wire)
    Check(
        fixtures.emplace(label, Fixture{{mid, Hex(plain)}, Hex(wire)}).second);
  Check(input.eof() && fixtures.size() == 24);
  NativeFixtures();
  Boundaries();
  std::cout << "Clan master transfer codec: " << checks << " checks passed\n";
  return checks;
}
