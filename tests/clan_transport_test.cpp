#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <stdexcept>
#include <type_traits>

#include "protocol/clans/clan_messages.h"

namespace proto = bubble_fighter::server::protocol;
namespace {
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Clan transport line " +
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
  std::vector<std::uint8_t> result;
  if (text == "-") return result;
  Check(text.size() % 2 == 0);
  for (std::size_t i = 0; i < text.size(); i += 2)
    result.push_back(
        static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
  return result;
}
template <class Message, class Packet>
void Malformed(const Packet& packet) {
  auto wrong = packet;
  ++wrong.message_id;
  Rejects([&] { Message::Decode(wrong); });
  wrong = packet;
  wrong.payload.push_back(0);
  Rejects([&] { Message::Decode(wrong); });
  for (std::size_t i = 0; i < packet.payload.size(); ++i) {
    wrong = packet;
    wrong.payload.resize(i);
    Rejects([&] { Message::Decode(wrong); });
  }
  wrong.payload.resize(proto::ClanPacketCodec::kMaxPayloadSize + 1);
  Rejects([&] { Message::Decode(wrong); });
}
struct Fixture {
  proto::ClanPacket packet;
  std::vector<std::uint8_t> frame;
};
std::map<std::string, Fixture> fixtures;

void Messages() {
  using namespace proto;
  for (const auto heap : {false, true}) {
    ClanCreateRequest create{heap ? u"ABCDEFGHIJKL" : u"Alpha", 0x01020304,
                             heap ? u"原版独立战队协议测试说明" : u"联机队伍",
                             0x11223344, 0xa1b2c3d4};
    const auto& expected =
        fixtures.at(heap ? "create_heap" : "create_inline").packet;
    Check(create.Encode() == expected);
    Check(ClanCreateRequest::Decode(expected) == create);
    Malformed<ClanCreateRequest>(expected);
    ClanCredentials credentials{0x12345678,
                                heap ? u"ABCDEFGHIJKL" : u"汉A",
                                0x11223344,
                                0xa1b2c3d4,
                                {0, 255, 16, 58, 0}};
    const auto& login =
        fixtures.at(heap ? "login_heap" : "login_inline").packet;
    Check(ClanLoginRequest{credentials}.Encode() == login);
    Check(ClanLoginRequest::Decode(login).credentials == credentials);
    Malformed<ClanLoginRequest>(login);
  }
  for (const auto value : {1u, 0xffffffffu, 0xfffffffeu}) {
    const auto packet =
        fixtures.at("login_result_" + std::to_string(value)).packet;
    Check(ClanLoginResponse{value}.Encode() == packet);
    Check(ClanLoginResponse::Decode(packet).result == value);
    Malformed<ClanLoginResponse>(packet);
  }
  for (const auto value : {1u, 2u, 0xffffff9cu}) {
    const ClanCreateResponse response{value, 0x11223344, 0xa1b2c3d4};
    const auto packet =
        fixtures.at("create_result_" + std::to_string(value)).packet;
    Check(response.Encode() == packet);
    Check(ClanCreateResponse::Decode(packet) == response);
    Malformed<ClanCreateResponse>(packet);
  }
  Check(ClanHelloNotification{}.Encode() == fixtures.at("hello").packet);
  ClanHelloNotification::Decode(fixtures.at("hello").packet);
  Check(ClanHeartbeatRequest{}.Encode() == fixtures.at("heartbeat").packet);
  ClanHeartbeatRequest::Decode(fixtures.at("heartbeat").packet);
  Check(ClanProfileRequest{}.Encode() == fixtures.at("profile_query").packet);
  ClanProfileRequest::Decode(fixtures.at("profile_query").packet);
  Malformed<ClanHelloNotification>(ClanHelloNotification{}.Encode());
  Malformed<ClanHeartbeatRequest>(ClanHeartbeatRequest{}.Encode());
  Malformed<ClanProfileRequest>(ClanProfileRequest{}.Encode());
  // Main S21 has a counted string; clan C1005 has a NUL terminated string.
  const auto credentials =
      ClanLoginRequest::Decode(fixtures.at("login_inline").packet).credentials;
  const auto response = ClanPassportResponse{credentials}.Encode();
  Check(response.message_id == 21);
  Check(response.payload ==
        Hex("7856341202000000496c410044332211d4c3b2a10500000000ff103a00"));
  Check(ClanPassportResponse::Decode(response).credentials == credentials);
  Malformed<ClanPassportResponse>(response);
  Check(ClanPassportRequest{}.Encode().message_id == 25);
  Check(ClanPassportRequest{}.Encode().payload.empty());
  ClanPassportRequest::Decode({25, {}});
  Malformed<ClanPassportRequest>(ClanPassportRequest{}.Encode());
  for (const auto position : {std::size_t{28}, std::size_t{29}}) {
    auto bad = fixtures.at("login_heap").packet;
    // Remove the text terminator without adding a replacement. Scalar zeros
    // may be encountered, but the resulting exact field layout must fail.
    bad.payload.erase(bad.payload.begin() + position);
    Rejects([&] { ClanLoginRequest::Decode(bad); });
  }
  auto bad_count = fixtures.at("login_inline").packet;
  std::fill(bad_count.payload.begin() + 18, bad_count.payload.begin() + 22,
            std::uint8_t{255});
  Rejects([&] { ClanLoginRequest::Decode(bad_count); });
  auto bad_main = response;
  std::fill(bad_main.payload.begin() + 4, bad_main.payload.begin() + 8,
            std::uint8_t{255});
  Rejects([&] { ClanPassportResponse::Decode(bad_main); });
  ClanCreateRequest create;
  create.name = std::u16string(u"A\0B", 3);
  Rejects([&] { create.Encode(); });
  create.name.clear();
  create.introduction = std::u16string(32758, u'x');
  Rejects([&] { create.Encode(); });
  create.introduction.resize(32757);
  Check(create.Encode().payload.size() == 65530);
  Check(ClanCreateRequest::Decode(create.Encode()) == create);
  ClanLoginRequest large;
  large.credentials.passport.resize(65513);
  Check(large.Encode().payload.size() == 65531);
  Check(ClanLoginRequest::Decode(large.Encode()).credentials ==
        large.credentials);
  large.credentials.passport.push_back(0);
  Rejects([&] { large.Encode(); });
  large.credentials.passport.clear();
  large.credentials.text_at4 = std::u16string(u"A\0B", 3);
  Rejects([&] { large.Encode(); });
  Rejects([&] { ClanPassportResponse{large.credentials}.Encode(); });
  // Empty and unpaired code units remain wire values, not membership/name
  // validation. That business policy is intentionally outside these codecs.
  create = {};
  create.name.push_back(static_cast<char16_t>(0xd800));
  Check(ClanCreateRequest::Decode(create.Encode()) == create);
}

void Frames() {
  using namespace proto;
  static_assert(!std::is_convertible_v<ClanPacket, TcpPacket>);
  static_assert(!std::is_convertible_v<TcpPacket, ClanPacket>);
  std::vector<std::uint8_t> joined;
  std::vector<ClanPacket> packets;
  for (const auto& [label, fixture] : fixtures) {
    Check(ClanPacketCodec::Encode(fixture.packet) == fixture.frame);
    const auto result = ClanPacketCodec::Decode(fixture.frame);
    Check(result.status == ClanDecodeStatus::kComplete);
    Check(result.frame_size == fixture.frame.size());
    Check(result.packet == fixture.packet);
    joined.insert(joined.end(), fixture.frame.begin(), fixture.frame.end());
    packets.push_back(fixture.packet);
    if (fixture.frame.size() <= 72) {
      for (std::size_t split = 0; split <= fixture.frame.size(); ++split) {
        ClanStreamDecoder decoder;
        auto actual = decoder.Feed(std::span(fixture.frame).first(split));
        auto tail = decoder.Feed(std::span(fixture.frame).subspan(split));
        actual.insert(actual.end(), tail.begin(), tail.end());
        Check(actual == std::vector<ClanPacket>{fixture.packet});
        Check(!decoder.HasPendingData());
        decoder.Finish();
      }
    }
  }
  for (const auto chunk :
       {std::size_t{1}, std::size_t{37}, std::size_t{65535}, joined.size()}) {
    ClanStreamDecoder decoder;
    std::vector<ClanPacket> actual;
    for (std::size_t i = 0; i < joined.size(); i += chunk) {
      const auto batch = decoder.Feed(
          std::span(joined).subspan(i, std::min(chunk, joined.size() - i)));
      actual.insert(actual.end(), batch.begin(), batch.end());
    }
    decoder.Finish();
    Check(actual == packets);
    Check(!decoder.HasPendingData());
    Rejects([&] { decoder.Feed({}); });
  }
  const auto frame = fixtures.at("create_inline").frame;
  for (std::size_t size = 1; size < frame.size(); ++size) {
    const auto partial = std::span(frame).first(size);
    Check(ClanPacketCodec::Decode(partial).status ==
          ClanDecodeStatus::kNeedMoreData);
    ClanStreamDecoder decoder;
    Check(decoder.Feed(partial).empty());
    Check(decoder.HasPendingData());
    Rejects([&] { decoder.Finish(); });
    Check(decoder.IsFailed() && !decoder.HasPendingData());
    Rejects([&] { decoder.Feed(frame); });
    Rejects([&] { decoder.Finish(); });
  }
  for (const auto length : {0xfffcu, 0xfffdu, 0xfffeu, 0xffffu}) {
    const std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>(length & 255), 255, 1, 4};
    Check(ClanPacketCodec::Decode(header).status ==
          ClanDecodeStatus::kPayloadTooLarge);
    ClanStreamDecoder decoder;
    Check(decoder.Feed(std::span(header).first(3)).empty());
    Rejects([&] { decoder.Feed(std::span(header).last(1)); });
    Check(decoder.IsFailed() && !decoder.HasPendingData());
    Rejects([&] { decoder.Feed(frame); });
    Rejects([&] {
      ClanPacketCodec::Encode({1005, std::vector<std::uint8_t>(length)});
    });
  }
  const auto largest = fixtures.at("size_65531");
  Check(largest.frame.size() == 65535);
  Check(ClanPacketCodec::Decode(std::span(largest.frame).first(4)).frame_size ==
        65535);
  Check(ClanPacketCodec::Decode(joined).frame_size ==
        fixtures.begin()->second.frame.size());
  // Alteration is not detectable by this native protocol. Authentication
  // must validate the decoded Passport independently of this transform.
  auto modified = frame;
  modified.back() ^= 1;
  const auto corrupted = ClanPacketCodec::Decode(modified);
  Check(corrupted.status == ClanDecodeStatus::kComplete);
  Check(corrupted.packet.payload !=
        fixtures.at("create_inline").packet.payload);
}
}  // namespace

int RunClanTransportTests(const std::filesystem::path& directory) {
  std::ifstream input(directory / "clan_packets.txt");
  Check(input.good());
  std::string label, payload, frame;
  std::uint16_t id = 0;
  while (input >> label >> id >> payload >> frame)
    Check(fixtures.emplace(label, Fixture{{id, Hex(payload)}, Hex(frame)})
              .second);
  Check(input.eof() && fixtures.size() == 276);
  Messages();
  Frames();
  std::cout << "Clan transport: " << checks << " checks passed\n";
  return checks;
}
