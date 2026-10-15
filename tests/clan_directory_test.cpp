#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <span>
#include <stdexcept>

#include "protocol/clans/clan_directory_messages.h"
#include "sessions/clan_session.h"

namespace proto = bubble_fighter::server::protocol;
namespace {
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Clan directory line " +
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
    auto tail = decoder.Feed(frame.subspan(split));
    packets.insert(packets.end(), tail.begin(), tail.end());
    decoder.Finish();
    Check(packets == std::vector<proto::ClanPacket>{fixture.packet});
  }
}
void FixedPackets() {
  for (const auto mode : {4u, 5u, 6u, 7u, 8u})
    Verify("query_" + std::to_string(mode),
           proto::ClanDirectoryRequest{mode, 1, mode == 4 ? u"Alpha" : u""});
  Verify("query_wide",
         proto::ClanDirectoryRequest{0xffffffff, 0x87654321, u"水泡🚀Alpha"});
  Verify("query_zero", proto::ClanDirectoryRequest{});
  const proto::ClanDirectoryRecord normal{71,
                                          u"Alpha",
                                          11,
                                          u"Master",
                                          12,
                                          13,
                                          0x0102030405060708ULL,
                                          0x1112131415161718ULL,
                                          1,
                                          14,
                                          15,
                                          u"Hello",
                                          0x40f1d5b000000000ULL,
                                          0x2122232425262728ULL,
                                          16};
  const proto::ClanDirectoryRecord wide{0xffffffff,
                                        u"水泡🚀Alpha",
                                        0x87654321,
                                        u"管理🚀Master",
                                        0x80000000,
                                        0xffffffff,
                                        0x7ff8000012345678ULL,
                                        0xffffffffffffffffULL,
                                        2,
                                        0x12345678,
                                        0xfedcba98,
                                        u"Long introduction 水泡🚀",
                                        0x8000000000000000ULL,
                                        0x7ff80000deadbeefULL,
                                        0xffffffff};
  Verify("empty", proto::ClanDirectoryResponse{0x76543210, 3, u"", {}});
  Verify("zero", proto::ClanDirectoryResponse{0x76543210, 3, u"", {{}}});
  Verify("normal", proto::ClanDirectoryResponse{0x76543210, 3, u"", {normal}});
  Verify("wide",
         proto::ClanDirectoryResponse{0x76543210, 3, u"水泡🚀Header", {wide}});
  Verify("multiple", proto::ClanDirectoryResponse{
                         0x76543210, 3, u"水泡🚀Header", {normal, wide}});
  Verify("five", proto::ClanDirectoryResponse{
                     0x76543210, 3, u"",
                     std::vector<proto::ClanDirectoryRecord>(5, normal)});
  Verify("six",
         proto::ClanDirectoryResponse{
             0x76543210, 3, u"", std::vector<proto::ClanDirectoryRecord>(6)});
}
void Boundaries() {
  const std::u16string invalid{u'A', u'\0', u'B'};
  const std::u16string surrogates{char16_t{0xd800}, u'A', char16_t{0xdfff}};
  proto::ClanDirectoryRequest query{4, 1, invalid};
  Rejects([&] { query.Encode(); });
  query.search_text = surrogates;
  Check(proto::ClanDirectoryRequest::Decode(query.Encode()) == query);
  query.search_text.assign((65530 - 10) / 2, u'A');
  Check(query.Encode().payload.size() == 65530);
  Check(proto::ClanDirectoryRequest::Decode(query.Encode()) == query);
  query.search_text.push_back(u'A');
  Rejects([&] { query.Encode(); });
  auto packet = proto::ClanDirectoryRequest{4, 1, {}}.Encode();
  packet.payload.back() = 1;
  Rejects([&] { proto::ClanDirectoryRequest::Decode(packet); });
  proto::ClanDirectoryResponse result;
  result.text_at12 = invalid;
  Rejects([&] { result.Encode(); });
  result.text_at12 = surrogates;
  Check(proto::ClanDirectoryResponse::Decode(result.Encode()) == result);
  result.text_at12.assign((65530 - 14) / 2, u'A');
  Check(result.Encode().payload.size() == 65530);
  Check(proto::ClanDirectoryResponse::Decode(result.Encode()) == result);
  result.clans.emplace_back();
  Rejects([&] { result.Encode(); });
  result.clans.clear();
  result.text_at12.push_back(u'A');
  Rejects([&] { result.Encode(); });
  for (const auto field : {&proto::ClanDirectoryRecord::clan_name,
                           &proto::ClanDirectoryRecord::master_name,
                           &proto::ClanDirectoryRecord::introduction}) {
    result = {};
    result.clans.resize(1);
    auto& text = result.clans[0].*field;
    text = invalid;
    Rejects([&] { result.Encode(); });
    text = surrogates;
    Check(proto::ClanDirectoryResponse::Decode(result.Encode()) == result);
    text.assign(65531, u'A');
    Rejects([&] { result.Encode(); });
    text.assign((65530 - 14 - 70) / 2, u'A');
    Check(result.Encode().payload.size() == 65530);
    Check(proto::ClanDirectoryResponse::Decode(result.Encode()) == result);
    text.push_back(u'A');
    Rejects([&] { result.Encode(); });
  }
  // Charge all variable strings together, including the header and later rows.
  result = {};
  result.clans.resize(2);
  result.text_at12.assign(10000, u'H');
  result.clans[0].clan_name.assign(5000, u'A');
  result.clans[0].master_name.assign(5000, u'B');
  result.clans[0].introduction.assign(5000, u'C');
  result.clans[1].introduction.assign((65530 - 14 - 140) / 2 - 25000, u'D');
  Check(result.Encode().payload.size() == 65530);
  Check(proto::ClanDirectoryResponse::Decode(result.Encode()) == result);
  result.clans[1].introduction.push_back(u'D');
  Rejects([&] { result.Encode(); });
  result = {};
  result.clans.resize((65531 - 14) / 70);
  packet = result.Encode();
  Check(result.clans.size() == 935);
  Check(proto::ClanDirectoryResponse::Decode(packet) == result);
  result.text_at12.assign((65530 - packet.payload.size()) / 2, u'A');
  Check(result.Encode().payload.size() == 65530);
  result.clans.emplace_back();
  Rejects([&] { result.Encode(); });
  result = {};
  result.clans.resize(1);
  for (const auto count : {0u, 2u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
    packet = result.Encode();
    for (std::size_t i = 0; i < 4; ++i)
      packet.payload[i] = static_cast<std::uint8_t>(count >> (8 * i));
    Rejects([&] { proto::ClanDirectoryResponse::Decode(packet); });
  }
  packet = result.Encode();
  packet.payload.resize(14);
  std::fill(packet.payload.begin(), packet.payload.begin() + 4,
            std::uint8_t{255});
  Rejects([&] { proto::ClanDirectoryResponse::Decode(packet); });
  // Missing terminators at the header and each successive row text.
  for (const std::size_t offset : {12u, 18u, 24u, 60u}) {
    packet = result.Encode();
    std::fill(packet.payload.begin() + static_cast<std::ptrdiff_t>(offset),
              packet.payload.end(), std::uint8_t{255});
    Rejects([&] { proto::ClanDirectoryResponse::Decode(packet); });
  }
}
void DirectorySessions() {
  using Auth = bubble_fighter::server::features::clans::ClanAuthentication;
  using Identity = bubble_fighter::server::features::clans::ClanIdentity;
  using Snapshot = bubble_fighter::server::features::clans::ClanSnapshot;
  using Session = bubble_fighter::server::sessions::ClanSession;
  const proto::ClanDirectoryRequest query{4, 1, u"Alpha"};
  for (int scenario = 0; scenario < 7; ++scenario) {
    auto auth = std::make_shared<Auth>();
    auto game = auth->BindGame({44, 71});
    int calls = 0;
    proto::ClanDirectoryResponse expected{0, 1, u"", {}};
    auto provider = [&](const Identity& identity,
                        const proto::ClanDirectoryRequest& request)
        -> std::optional<proto::ClanDirectoryResponse> {
      ++calls;
      Check(identity == Identity{44, 71} && request == query);
      if (scenario == 1) return {};
      if (scenario == 2) game.reset();
      if (scenario == 3)
        throw std::runtime_error("Synthetic directory failure");
      auto value = expected;
      if (scenario == 4) value.text_at12.assign(65531, u'X');
      return value;
    };
    Session session(
        auth, [](const Identity&) { return std::optional{Snapshot{}}; },
        Auth::Clock::now, {}, provider);
    if (scenario == 5) {
      Rejects([&] { session.Process(query.Encode()); });
      Check(calls == 0 && session.Poll().close_connection);
      continue;
    }
    const auto login = session.Process(
        proto::ClanLoginRequest{auth->Issue(game, u"Fighter")}.Encode());
    Check(!login.close_connection &&
          proto::ClanLoginResponse::Decode(login.responses.at(0)).result == 1);
    if (scenario == 6) game.reset();
    if (scenario == 3 || scenario == 4) {
      Rejects([&] { session.Process(query.Encode()); });
      Check(session.Poll().close_connection && calls == 1);
    } else {
      const auto response = session.Process(query.Encode());
      Check(calls == (scenario == 6 ? 0 : 1));
      Check(response.close_connection == (scenario != 0));
      if (scenario == 0) {
        Check(response.responses ==
              std::vector<proto::ClanPacket>{expected.Encode()});
        Check(!session.Process(proto::ClanProfileRequest{}.Encode())
                   .close_connection);
      } else {
        Check(response.responses.empty() && session.Poll().close_connection);
      }
    }
  }
  for (const auto& bad :
       std::vector<proto::ClanDirectoryRequest>{{4, 0, u"Alpha"},
                                                {4, 10001, u"Alpha"},
                                                {4, 0xffffffff, u"Alpha"},
                                                {4, 1, u""},
                                                {4, 1, u"1234567890123"},
                                                {0, 1, u""},
                                                {3, 1, u""},
                                                {9, 1, u""},
                                                {0xffffffff, 1, u""},
                                                {5, 1, u"Alpha"},
                                                {6, 1, u"Alpha"},
                                                {7, 1, u"Alpha"},
                                                {8, 1, u"Alpha"}}) {
    auto auth = std::make_shared<Auth>();
    auto game = auth->BindGame({44, 71});
    int calls = 0;
    Session session(
        auth, [](const Identity&) { return std::optional{Snapshot{}}; },
        Auth::Clock::now, {},
        [&](const auto&, const auto&) {
          ++calls;
          return std::optional{proto::ClanDirectoryResponse{}};
        });
    session.Process(
        proto::ClanLoginRequest{auth->Issue(game, u"Fighter")}.Encode());
    Rejects([&] { session.Process(bad.Encode()); });
    Check(calls == 0 && session.Poll().close_connection);
  }
  for (const auto mode : {4u, 5u, 6u, 7u, 8u})
    for (const auto page : {1u, 10000u})
      Check(bubble_fighter::server::features::clans::
                IsClanDirectoryQuerySupported(
                    {mode, page, mode == 4 ? u"Alpha" : u""}));
}
}  // namespace

int RunClanDirectoryTests(const std::filesystem::path& directory) {
  std::ifstream input(directory / "clan_directory_packets.txt");
  Check(input.good());
  std::string label, payload, frame;
  std::uint16_t id = 0;
  while (input >> label >> id >> payload >> frame)
    Check(fixtures.emplace(label, Fixture{{id, Hex(payload)}, Hex(frame)})
              .second);
  Check(input.eof());
  Check(fixtures.size() == 14);
  FixedPackets();
  Boundaries();
  DirectorySessions();
  std::cout << "Clan directory: " << checks << " checks passed\n";
  return checks;
}
