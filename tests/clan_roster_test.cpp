#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <span>
#include <stdexcept>

#include "protocol/clans/clan_roster_messages.h"

namespace proto = bubble_fighter::server::protocol;
namespace {
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Clan roster line " + std::to_string(at.line()));
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
    auto tail = decoder.Feed(frame.subspan(split));
    packets.insert(packets.end(), tail.begin(), tail.end());
    decoder.Finish();
    Check(packets == std::vector<proto::ClanPacket>{fixture.packet});
  }
}
void FixedLists() {
  const proto::ClanMemberRecord normal{71, u"Fighter", 11, 12, 13};
  const proto::ClanMemberRecord wide{0xffffffff, u"水泡🚀Alpha", 0xffffffff,
                                     0xffffffec, 0x87654321};
  const proto::ClanMemberDetailsRecord normal_details{71,
                                                      u"Fighter",
                                                      11,
                                                      12,
                                                      13,
                                                      14,
                                                      0x0102030405060708ULL,
                                                      0x1112131415161718ULL,
                                                      0x2122232425262728ULL,
                                                      15,
                                                      16,
                                                      17,
                                                      18};
  const proto::ClanMemberDetailsRecord wide_details{0xffffffff,
                                                    u"水泡🚀Alpha",
                                                    0xffffffff,
                                                    0xffffffec,
                                                    0x87654321,
                                                    0x12345678,
                                                    0xffffffffffffffffULL,
                                                    0x8000000000000000ULL,
                                                    0x7ff8000012345678ULL,
                                                    0x80818283,
                                                    0x90919293,
                                                    0xa0a1a2a3,
                                                    0xb0b1b2b3};
  const proto::ClanApplicantRecord normal_applicant{
      71, u"Fighter", 11, 12, 0x0102030405060708ULL, 13, 14, 15, 16};
  const proto::ClanApplicantRecord wide_applicant{
      0xffffffff, u"水泡🚀Alpha", 0xffffffff, 0xffffffec, 0xffffffffffffffffULL,
      0x87654321, 0x12345678,     0x80818283, 0x90919293};
  Verify("empty_1012", proto::ClanRosterResponse{});
  Verify("zero_1012", proto::ClanRosterResponse{{proto::ClanMemberRecord{}}});
  Verify("normal_1012", proto::ClanRosterResponse{{normal}});
  Verify("wide_1012", proto::ClanRosterResponse{{wide}});
  Verify("multiple_1012", proto::ClanRosterResponse{{normal, wide}});
  Verify("empty_1014", proto::ClanMemberDetailsResponse{0x76543210, {}});
  Verify("zero_1014", proto::ClanMemberDetailsResponse{
                          0x76543210, {proto::ClanMemberDetailsRecord{}}});
  Verify("normal_1014",
         proto::ClanMemberDetailsResponse{0x76543210, {normal_details}});
  Verify("wide_1014",
         proto::ClanMemberDetailsResponse{0x76543210, {wide_details}});
  Verify("multiple_1014", proto::ClanMemberDetailsResponse{
                              0x76543210, {normal_details, wide_details}});
  Verify("empty_1070", proto::ClanApplicantsResponse{});
  Verify("zero_1070",
         proto::ClanApplicantsResponse{{proto::ClanApplicantRecord{}}});
  Verify("normal_1070", proto::ClanApplicantsResponse{{normal_applicant}});
  Verify("wide_1070", proto::ClanApplicantsResponse{{wide_applicant}});
  Verify("multiple_1070",
         proto::ClanApplicantsResponse{{normal_applicant, wide_applicant}});
}
void FixedQueries() {
  Verify("query_1011", proto::ClanRosterRequest{});
  Verify("query_1013", proto::ClanMemberDetailsRequest{});
  Verify("query_1069", proto::ClanApplicantsRequest{});
  for (auto mode : {0u, 1u, 2u, 0xffffffffu})
    Verify("recruit_" + std::to_string(mode),
           proto::ClanRecruitmentRequest{71, mode});
  Verify("result_0_0", proto::ClanRecruitmentResponse{});
  for (auto mode : {0u, 1u, 2u})
    Verify("result_1_" + std::to_string(mode),
           proto::ClanRecruitmentResponse{71, 1, mode});
  Verify("result_4294967295_4294967294",
         proto::ClanRecruitmentResponse{0xffffffff, 0xffffffff, 0xfffffffe});
  const proto::ClanPacket opaque{1013, {255, 255, 255, 255}};
  Check(proto::ClanMemberDetailsRequest::Decode(opaque).value_at0 ==
        0xffffffff);
  Check(proto::ClanMemberDetailsRequest{0xffffffff}.Encode() == opaque);
  Check(proto::ClanRecruitmentRequest::Decode(
            {1102, std::vector<std::uint8_t>(8)}) ==
        proto::ClanRecruitmentRequest{});
}
template <class Message, class Row>
void ListBoundaries(std::vector<Row> Message::* field, std::size_t prefix,
                    std::size_t minimum_row) {
  Message message;
  auto& rows = message.*field;
  rows.resize(1);
  rows[0].character_name = std::u16string{u'A', u'\0', u'B'};
  Rejects([&] { message.Encode(); });
  rows[0].character_name.assign(65531, u'A');
  Rejects([&] { message.Encode(); });
  rows[0].character_name.assign((65530 - prefix - minimum_row) / 2, u'A');
  Check(message.Encode().payload.size() == 65530);
  Check(Message::Decode(message.Encode()) == message);
  rows[0].character_name.push_back(u'A');
  Rejects([&] { message.Encode(); });
  rows.assign(2, Row{});
  const auto units = (65530 - prefix - 2 * minimum_row) / 2;
  rows[0].character_name.assign(units / 2, u'A');
  rows[1].character_name.assign(units - units / 2, u'B');
  Check(message.Encode().payload.size() == 65530);
  Check(Message::Decode(message.Encode()) == message);
  rows[1].character_name.push_back(u'B');
  Rejects([&] { message.Encode(); });
  rows.assign((65531 - prefix) / minimum_row, Row{});
  auto packet = message.Encode();
  Check(packet.payload.size() <= 65531);
  Check(Message::Decode(packet) == message);
  rows[0].character_name.assign((65530 - packet.payload.size()) / 2, u'A');
  Check(message.Encode().payload.size() == 65530);
  rows.push_back(Row{});
  Rejects([&] { message.Encode(); });
  rows.assign(1, Row{});
  rows[0].character_name =
      std::u16string{char16_t{0xd800}, u'A', char16_t{0xdfff}};
  Check(Message::Decode(message.Encode()) == message);
  for (const auto count : {0u, 2u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
    packet = message.Encode();
    for (std::size_t i = 0; i < 4; ++i)
      packet.payload[prefix - 4 + i] =
          static_cast<std::uint8_t>(count >> (8 * i));
    Rejects([&] { Message::Decode(packet); });
  }
  rows.assign(1, Row{});
  packet = message.Encode();
  std::fill(packet.payload.begin() + static_cast<std::ptrdiff_t>(prefix),
            packet.payload.end(), std::uint8_t{255});
  Rejects([&] { Message::Decode(packet); });
  // A huge count with a tiny payload must fail before vector allocation.
  packet.payload.resize(prefix);
  std::fill(packet.payload.begin() + static_cast<std::ptrdiff_t>(prefix - 4),
            packet.payload.end(), std::uint8_t{255});
  Rejects([&] { Message::Decode(packet); });
}
}  // namespace

int RunClanRosterTests(const std::filesystem::path& directory) {
  std::ifstream input(directory / "clan_roster_packets.txt");
  Check(input.good());
  std::string label, payload, frame;
  std::uint16_t id = 0;
  while (input >> label >> id >> payload >> frame)
    Check(fixtures.emplace(label, Fixture{{id, Hex(payload)}, Hex(frame)})
              .second);
  Check(input.eof());
  Check(fixtures.size() == 27);
  FixedLists();
  FixedQueries();
  ListBoundaries(&proto::ClanRosterResponse::members, 4, 18);
  ListBoundaries(&proto::ClanMemberDetailsResponse::members, 8, 62);
  ListBoundaries(&proto::ClanApplicantsResponse::applicants, 4, 38);
  std::cout << "Clan roster: " << checks << " checks passed\n";
  return checks;
}
