#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <span>
#include <stdexcept>

#include "protocol/clans/clan_messages.h"

namespace proto = bubble_fighter::server::protocol;
namespace {
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Clan profile line " + std::to_string(at.line()));
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
  if (text.size() % 2) throw std::runtime_error("Odd fixture hex length");
  std::vector<std::uint8_t> bytes;
  bytes.reserve(text.size() / 2);
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
  bad = fixture.packet;
  bad.payload.resize(proto::ClanPacketCodec::kMaxPayloadSize + 1);
  Rejects([&] { Message::Decode(bad); });
  // Exhaustively truncate short native fixtures. Large fixtures use each
  // scalar edge and end-of-frame boundaries without quadratic copying.
  for (std::size_t size = 0; size < fixture.packet.payload.size(); ++size) {
    if (fixture.packet.payload.size() > 1024 && size > 20 &&
        size + 20 < fixture.packet.payload.size())
      continue;
    bad = fixture.packet;
    bad.payload.resize(size);
    Rejects([&] { Message::Decode(bad); });
  }
  for (std::size_t split : {std::size_t{0}, std::size_t{1}, std::size_t{3},
                            std::size_t{4}, fixture.frame.size() / 2,
                            fixture.frame.size() - 1, fixture.frame.size()}) {
    proto::ClanStreamDecoder decoder;
    const std::span<const std::uint8_t> frame = fixture.frame;
    auto packets = decoder.Feed(frame.first(split));
    auto tail = decoder.Feed(frame.subspan(split));
    packets.insert(packets.end(), tail.begin(), tail.end());
    decoder.Finish();
    Check(packets == std::vector<proto::ClanPacket>{fixture.packet});
  }
}

void Profiles() {
  for (const auto status :
       {0u, 1u, 100u, 101u, 102u, 0xffffffecu, 0xffffffe2u, 0xffffffd8u,
        0xffffffc4u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
    Verify("profile_" + std::to_string(status),
           proto::ClanProfileResponse{255, 0x12345678, status, u"战队AB",
                                      0x40e0000011223344ULL});
  }
  Verify("profile_empty", proto::ClanProfileResponse{});
  Verify("profile_heap",
         proto::ClanProfileResponse{1, 1234, 101, u"ABCDEFGHIJKL",
                                    0x7ff8000012345678ULL});
  const std::u16string surrogates{char16_t{0xd800}, u'A', char16_t{0xdfff}};
  Verify("profile_surrogates",
         proto::ClanProfileResponse{1, 99, 100, surrogates,
                                    0x8000000000000000ULL});
  proto::ClanProfileResponse capacity{1, 1, 100, std::u16string(32756, u'A'),
                                      0xffffffffffffffffULL};
  Verify("profile_capacity", capacity);
  Check(capacity.Encode().payload.size() == 65531);
  capacity.clan_name.push_back(u'A');
  Rejects([&] { capacity.Encode(); });
  capacity.clan_name = std::u16string{u'A', u'\0', u'B'};
  Rejects([&] { capacity.Encode(); });
  auto missing_terminator = fixtures.at("profile_heap").packet;
  for (std::size_t i = 9; i < missing_terminator.payload.size(); ++i)
    missing_terminator.payload[i] = 255;
  Rejects([&] { proto::ClanProfileResponse::Decode(missing_terminator); });
}

void Details() {
  Verify("details_empty", proto::ClanDetailsResponse{});
  for (const bool heap : {false, true}) {
    const proto::ClanDetailsResponse expected{
        heap ? u"ABCDEFGHIJKL" : u"Clan",
        0x01020304,
        heap ? u"1234567890" : u"汉A",
        0x11121314,
        heap ? u"ABCDEFGHI" : u"副会长",
        0x21222324,
        0x31323334,
        0x4142434445464748ULL,
        0x5152535455565758ULL,
        0x61626364,
        0x71727374,
        0x81828384,
        heap ? u"详细介绍文字内容" : u"简介",
        heap ? u"公告文字内容测试" : u"公告",
        0x91929394,
        0xa1a2a3a4};
    Verify(heap ? "details_heap" : "details_inline", expected);
  }
  proto::ClanDetailsResponse capacity;
  capacity.text_at200.assign(32734, u'A');
  Verify("details_capacity", capacity);
  Check(capacity.Encode().payload.size() == 65530);
  capacity.text_at200.push_back(u'A');
  Rejects([&] { capacity.Encode(); });
  // Capacity accounts for the sum of all five strings, not only each one.
  capacity.text_at200.assign(16367, u'A');
  capacity.clan_name.assign(16367, u'A');
  Check(capacity.Encode().payload.size() == 65530);
  capacity.text_at76.push_back(u'A');
  Rejects([&] { capacity.Encode(); });

  using Field = std::u16string proto::ClanDetailsResponse::*;
  const std::array<Field, 5> fields{&proto::ClanDetailsResponse::clan_name,
                                    &proto::ClanDetailsResponse::text_at76,
                                    &proto::ClanDetailsResponse::text_at100,
                                    &proto::ClanDetailsResponse::text_at176,
                                    &proto::ClanDetailsResponse::text_at200};
  for (const auto field : fields) {
    proto::ClanDetailsResponse message;
    message.*field = std::u16string{u'A', u'\0', u'B'};
    Rejects([&] { message.Encode(); });
    (message.*field).assign(32735, u'A');
    Rejects([&] { message.Encode(); });
    (message.*field).assign(65531, u'A');
    Rejects([&] { message.Encode(); });
  }
  auto missing_terminators = fixtures.at("details_inline").packet;
  std::fill(missing_terminators.payload.begin(),
            missing_terminators.payload.end(), std::uint8_t{255});
  Rejects([&] { proto::ClanDetailsResponse::Decode(missing_terminators); });
  const auto query = fixtures.at("details_query");
  Check(proto::ClanDetailsRequest{}.Encode() == query.packet);
  Check(proto::ClanPacketCodec::Encode(query.packet) == query.frame);
  proto::ClanDetailsRequest::Decode(query.packet);
  Rejects([] { proto::ClanDetailsRequest::Decode({1015, {}}); });
  Rejects([] { proto::ClanDetailsRequest::Decode({1017, {0}}); });
}
}  // namespace

int RunClanProfileTests(const std::filesystem::path& directory) {
  std::ifstream input(directory / "clan_profile_packets.txt");
  Check(input.good());
  std::string name, payload, frame;
  std::uint16_t id = 0;
  while (input >> name >> id >> payload >> frame)
    Check(
        fixtures.emplace(name, Fixture{{id, Hex(payload)}, Hex(frame)}).second);
  Check(input.eof());
  Check(fixtures.size() == 21);
  Profiles();
  Details();
  std::cout << "Clan profile: " << checks << " checks passed\n";
  return checks;
}
