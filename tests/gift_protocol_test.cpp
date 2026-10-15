#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <stdexcept>
#include <string>

#include "protocol/gifts/gift_messages.h"

namespace {
namespace p = bubble_fighter::server::protocol;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Gift protocol line " + std::to_string(at.line()));
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
template <class T>
void Fixed(const T& expected, const p::TcpPacket& packet) {
  Check(T::Decode(packet) == expected);
  const auto encoded = expected.Encode();
  Check(encoded.message_id == packet.message_id &&
        encoded.payload == packet.payload);
  for (std::size_t size = 0; size < packet.payload.size(); ++size) {
    auto bad = packet;
    bad.payload.resize(size);
    Rejects([&] { (void)T::Decode(bad); });
  }
  auto bad = packet;
  bad.payload.push_back(0);
  Rejects([&] { (void)T::Decode(bad); });
  bad = packet;
  ++bad.message_id;
  Rejects([&] { (void)T::Decode(bad); });
  bad = packet;
  bad.payload.resize(p::kMaxGiftPayloadBytes + 1);
  Rejects([&] { (void)T::Decode(bad); });
}
template <class T>
void Roundtrip(const T& value) {
  Check(T::Decode(value.Encode()) == value);
}
void Put(std::vector<std::uint8_t>& bytes, std::size_t at,
         std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    bytes.at(at + i) = static_cast<std::uint8_t>(value >> (i * 8));
}
}  // namespace

int RunGiftProtocolTests(const std::filesystem::path& directory) {
  checks = 0;
  std::ifstream input(directory / "gift_protocol_packets.txt");
  Check(input.good());
  std::map<std::string, p::TcpPacket> fixtures;
  std::string label, hex;
  std::uint16_t mid = 0;
  while (input >> label >> mid >> hex) {
    p::TcpPacket packet{mid, {}};
    Check(hex.size() % 2 == 0);
    for (std::size_t i = 0; i < hex.size(); i += 2)
      packet.payload.push_back(
          static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    Check(fixtures.emplace(label, std::move(packet)).second);
  }
  Check(fixtures.size() == 40);
  // Expected objects come from the independent cases fixed before codec
  // implementation.
  Fixed(p::GiftReceivedListRequest{0x0U, 0x0U},
        fixtures.at("GiftReceivedListRequest_zero"));
  Fixed(p::GiftSentListRequest{0x0U, 0x0U},
        fixtures.at("GiftSentListRequest_zero"));
  Fixed(p::GiftSendRequest{std::u16string{}, std::u16string{}, 0x0U,
                           std::u16string{}},
        fixtures.at("GiftSendRequest_zero"));
  Fixed(p::GiftReceiveRequest{0x0U}, fixtures.at("GiftReceiveRequest_zero"));
  Fixed(p::GiftDiscardRequest{0x0U}, fixtures.at("GiftDiscardRequest_zero"));
  Fixed(p::GiftViewThanksRequest{0x0U},
        fixtures.at("GiftViewThanksRequest_zero"));
  Fixed(p::GiftDeleteSentRequest{0x0U},
        fixtures.at("GiftDeleteSentRequest_zero"));
  Fixed(p::GiftDeleteReceivedRequest{0x0U},
        fixtures.at("GiftDeleteReceivedRequest_zero"));
  Fixed(p::GiftSendThanksRequest{0x0U, std::u16string{}, 0x0U},
        fixtures.at("GiftSendThanksRequest_zero"));
  Fixed(p::GiftReceivedListResponse{0x0U, 0x0U, 0x0U, {}},
        fixtures.at("GiftReceivedListResponse_zero"));
  Fixed(p::GiftSentListResponse{0x0U, 0x0U, {}},
        fixtures.at("GiftSentListResponse_zero"));
  Fixed(p::GiftNewReceiveNotification{0x0U, 0x0U},
        fixtures.at("GiftNewReceiveNotification_zero"));
  Fixed(p::GiftSendResponse{std::u16string{}, 0x0U, 0x0U, std::u16string{},
                            std::u16string{}},
        fixtures.at("GiftSendResponse_zero"));
  Fixed(p::GiftReceiveResponse{0x0U, 0x0U, 0x0U, 0x0U},
        fixtures.at("GiftReceiveResponse_zero"));
  Fixed(p::GiftDiscardResponse{0x0U, 0x0U, 0x0U},
        fixtures.at("GiftDiscardResponse_zero"));
  Fixed(p::GiftDeleteReceivedResponse{0x0U, 0x0U, 0x0U},
        fixtures.at("GiftDeleteReceivedResponse_zero"));
  Fixed(p::GiftSendThanksResponse{0x0U},
        fixtures.at("GiftSendThanksResponse_zero"));
  Fixed(p::GiftConfirmResponse{0x0U}, fixtures.at("GiftConfirmResponse_zero"));
  Fixed(p::GiftDeleteSentResponse{0x0U, 0x0U},
        fixtures.at("GiftDeleteSentResponse_zero"));
  Fixed(p::GiftViewThanksResponse{0x0U, 0x0U, std::u16string{},
                                  std::u16string{}, 0x0U, 0x0U,
                                  p::GiftDetails{0x0U, {}, {}, {}, {}, {}}},
        fixtures.at("GiftViewThanksResponse_zero"));
  Fixed(p::GiftReceivedListRequest{0x87654321U, 0xFEDCBA98U},
        fixtures.at("GiftReceivedListRequest_distinct"));
  Fixed(p::GiftSentListRequest{0x87654321U, 0xFEDCBA98U},
        fixtures.at("GiftSentListRequest_distinct"));
  Fixed(
      p::GiftSendRequest{
          std::u16string{
              static_cast<char16_t>(0x0052), static_cast<char16_t>(0x0065),
              static_cast<char16_t>(0x0063), static_cast<char16_t>(0x0065),
              static_cast<char16_t>(0x0069), static_cast<char16_t>(0x0076),
              static_cast<char16_t>(0x0065), static_cast<char16_t>(0x0072),
              static_cast<char16_t>(0x0038)},
          std::u16string{static_cast<char16_t>(0xD800)}, 0x87654321U,
          std::u16string{
              static_cast<char16_t>(0x0048), static_cast<char16_t>(0x0069),
              static_cast<char16_t>(0x6CE1), static_cast<char16_t>(0xD83C),
              static_cast<char16_t>(0xDF0A), static_cast<char16_t>(0x0000),
              static_cast<char16_t>(0x0058)}},
      fixtures.at("GiftSendRequest_distinct"));
  Fixed(p::GiftReceiveRequest{0x87654321U},
        fixtures.at("GiftReceiveRequest_distinct"));
  Fixed(p::GiftDiscardRequest{0x87654321U},
        fixtures.at("GiftDiscardRequest_distinct"));
  Fixed(p::GiftViewThanksRequest{0x87654321U},
        fixtures.at("GiftViewThanksRequest_distinct"));
  Fixed(p::GiftDeleteSentRequest{0x87654321U},
        fixtures.at("GiftDeleteSentRequest_distinct"));
  Fixed(p::GiftDeleteReceivedRequest{0x87654321U},
        fixtures.at("GiftDeleteReceivedRequest_distinct"));
  Fixed(
      p::GiftSendThanksRequest{
          0x87654321U,
          std::u16string{
              static_cast<char16_t>(0x0048), static_cast<char16_t>(0x0069),
              static_cast<char16_t>(0x6CE1), static_cast<char16_t>(0xD83C),
              static_cast<char16_t>(0xDF0A), static_cast<char16_t>(0x0000),
              static_cast<char16_t>(0x0058)},
          0xFEDCBA98U},
      fixtures.at("GiftSendThanksRequest_distinct"));
  Fixed(
      p::GiftReceivedListResponse{
          0x87654321U,
          0xFEDCBA98U,
          0x10203040U,
          {p::ReceivedGiftRecord{
               0x87654321U, 0xFEDCBA9876543210ULL, 0xFEDCBA98U,
               std::u16string{
                   static_cast<char16_t>(0x0052), static_cast<char16_t>(0x0065),
                   static_cast<char16_t>(0x0063), static_cast<char16_t>(0x0065),
                   static_cast<char16_t>(0x0069), static_cast<char16_t>(0x0076),
                   static_cast<char16_t>(0x0065), static_cast<char16_t>(0x0072),
                   static_cast<char16_t>(0x0038)},
               0x10203040U, 0xFFFEU,
               std::u16string{
                   static_cast<char16_t>(0x0048), static_cast<char16_t>(0x0069),
                   static_cast<char16_t>(0x6CE1), static_cast<char16_t>(0xD83C),
                   static_cast<char16_t>(0xDF0A), static_cast<char16_t>(0x0000),
                   static_cast<char16_t>(0x0058)},
               0x8000000000000001ULL},
           p::ReceivedGiftRecord{0x2U, 0xFEDCBA9876543210ULL, 0xFEDCBA98U,
                                 std::u16string{static_cast<char16_t>(0x0042),
                                                static_cast<char16_t>(0x006F),
                                                static_cast<char16_t>(0x0062)},
                                 0x10203040U, 0xFFFEU,
                                 std::u16string{static_cast<char16_t>(0x0042)},
                                 0x8000000000000001ULL}}},
      fixtures.at("GiftReceivedListResponse_distinct"));
  Fixed(
      p::GiftSentListResponse{
          0x87654321U,
          0xFEDCBA98U,
          {p::SentGiftRecord{
               0x87654321U, 0xFEDCBA9876543210ULL, 0x8000000000000001ULL,
               0xFEDCBA98U,
               std::u16string{
                   static_cast<char16_t>(0x0052), static_cast<char16_t>(0x0065),
                   static_cast<char16_t>(0x0063), static_cast<char16_t>(0x0065),
                   static_cast<char16_t>(0x0069), static_cast<char16_t>(0x0076),
                   static_cast<char16_t>(0x0065), static_cast<char16_t>(0x0072),
                   static_cast<char16_t>(0x0038)},
               0x10203040U, 0xFFFEU, 0x8001U,
               std::u16string{
                   static_cast<char16_t>(0x0048), static_cast<char16_t>(0x0069),
                   static_cast<char16_t>(0x6CE1), static_cast<char16_t>(0xD83C),
                   static_cast<char16_t>(0xDF0A), static_cast<char16_t>(0x0000),
                   static_cast<char16_t>(0x0058)},
               std::u16string{static_cast<char16_t>(0xD800)}},
           p::SentGiftRecord{0x2U, 0xFEDCBA9876543210ULL, 0x8000000000000001ULL,
                             0xFEDCBA98U,
                             std::u16string{static_cast<char16_t>(0x0042),
                                            static_cast<char16_t>(0x006F),
                                            static_cast<char16_t>(0x0062)},
                             0x10203040U, 0xFFFEU, 0x8001U,
                             std::u16string{static_cast<char16_t>(0x0042)},
                             std::u16string{static_cast<char16_t>(0x0059)}}}},
      fixtures.at("GiftSentListResponse_distinct"));
  Fixed(p::GiftNewReceiveNotification{0x87654321U, 0xFEDCBA98U},
        fixtures.at("GiftNewReceiveNotification_distinct"));
  Fixed(
      p::GiftSendResponse{
          std::u16string{
              static_cast<char16_t>(0x0052), static_cast<char16_t>(0x0065),
              static_cast<char16_t>(0x0063), static_cast<char16_t>(0x0065),
              static_cast<char16_t>(0x0069), static_cast<char16_t>(0x0076),
              static_cast<char16_t>(0x0065), static_cast<char16_t>(0x0072),
              static_cast<char16_t>(0x0038)},
          0x87654321U, 0xFEDCBA98U,
          std::u16string{
              static_cast<char16_t>(0x0048), static_cast<char16_t>(0x0069),
              static_cast<char16_t>(0x6CE1), static_cast<char16_t>(0xD83C),
              static_cast<char16_t>(0xDF0A), static_cast<char16_t>(0x0000),
              static_cast<char16_t>(0x0058)},
          std::u16string{static_cast<char16_t>(0xD800)}},
      fixtures.at("GiftSendResponse_distinct"));
  Fixed(p::GiftReceiveResponse{0x87654321U, 0xFEDCBA98U, 0x10203040U,
                               0xFFFFFFFFU},
        fixtures.at("GiftReceiveResponse_distinct"));
  Fixed(p::GiftDiscardResponse{0x87654321U, 0xFEDCBA98U, 0x10203040U},
        fixtures.at("GiftDiscardResponse_distinct"));
  Fixed(p::GiftDeleteReceivedResponse{0x87654321U, 0xFEDCBA98U, 0x10203040U},
        fixtures.at("GiftDeleteReceivedResponse_distinct"));
  Fixed(p::GiftSendThanksResponse{0x87654321U},
        fixtures.at("GiftSendThanksResponse_distinct"));
  Fixed(p::GiftConfirmResponse{0x87654321U},
        fixtures.at("GiftConfirmResponse_distinct"));
  Fixed(p::GiftDeleteSentResponse{0x87654321U, 0xFEDCBA98U},
        fixtures.at("GiftDeleteSentResponse_distinct"));
  Fixed(
      p::GiftViewThanksResponse{
          0x87654321U, 0xFEDCBA98U,
          std::u16string{
              static_cast<char16_t>(0x0052), static_cast<char16_t>(0x0065),
              static_cast<char16_t>(0x0063), static_cast<char16_t>(0x0065),
              static_cast<char16_t>(0x0069), static_cast<char16_t>(0x0076),
              static_cast<char16_t>(0x0065), static_cast<char16_t>(0x0072),
              static_cast<char16_t>(0x0038)},
          std::u16string{
              static_cast<char16_t>(0x0048), static_cast<char16_t>(0x0069),
              static_cast<char16_t>(0x6CE1), static_cast<char16_t>(0xD83C),
              static_cast<char16_t>(0xDF0A), static_cast<char16_t>(0x0000),
              static_cast<char16_t>(0x0058)},
          0x10203040U, 0xFFFFFFFFU,
          p::GiftDetails{0x10203040U,
                         {0x87654321U, 0xFEDCBA98U},
                         {0x10203040U},
                         {0x0U, 0xFFFFFFFFU},
                         {0x0U, 0xFFU, 0x80U, 0x1U},
                         {p::GiftDetailRecord{
                              0x87654321U,
                              0xFEDCBA98U,
                              {0x0U, 0xFFFFFFFFU, 0x80000000U},
                              0x10203040U,
                              std::u16string{static_cast<char16_t>(0x0048),
                                             static_cast<char16_t>(0x0069),
                                             static_cast<char16_t>(0x6CE1),
                                             static_cast<char16_t>(0xD83C),
                                             static_cast<char16_t>(0xDF0A),
                                             static_cast<char16_t>(0x0000),
                                             static_cast<char16_t>(0x0058)}},
                          p::GiftDetailRecord{
                              0x87654321U,
                              0xFEDCBA98U,
                              {},
                              0x10203040U,
                              std::u16string{static_cast<char16_t>(0x0041),
                                             static_cast<char16_t>(0x0042),
                                             static_cast<char16_t>(0x0043),
                                             static_cast<char16_t>(0x0044),
                                             static_cast<char16_t>(0x0045),
                                             static_cast<char16_t>(0x0046),
                                             static_cast<char16_t>(0x0047)}}}}},
      fixtures.at("GiftViewThanksResponse_distinct"));

  for (std::size_t length : {0U, 7U, 8U, 1023U, 1024U, 16384U}) {
    const auto text = std::u16string(length, u'X');
    Roundtrip(p::GiftSendRequest{text, text, 0xFFFFFFFF, text});
    Roundtrip(p::GiftSendResponse{text, 0x80000000, 0xFFFFFFFF, text, text});
    Roundtrip(p::GiftSendThanksRequest{0xFFFFFFFF, text, 0xFFFF8000});
    p::GiftViewThanksResponse response;
    response.text_at8 = text;
    response.text_at32 = text;
    response.extra.records_at52.push_back({1, 2, {3, 4}, 5, text});
    Roundtrip(response);
  }
  const auto oversized = std::u16string(16385, u'X');
  Rejects([&] { (void)p::GiftSendRequest{oversized, {}, 1, {}}.Encode(); });
  Rejects([&] { (void)p::GiftSendThanksRequest{1, oversized, 0}.Encode(); });
  for (const std::uint32_t count : {1025U, 0xFFFFFFFFU}) {
    auto received = fixtures.at("GiftReceivedListResponse_zero");
    Put(received.payload, 12, count);
    Rejects([&] { (void)p::GiftReceivedListResponse::Decode(received); });
    auto sent = fixtures.at("GiftSentListResponse_zero");
    Put(sent.payload, 8, count);
    Rejects([&] { (void)p::GiftSentListResponse::Decode(sent); });
    for (const std::size_t offset : {28U, 32U, 36U, 40U, 44U}) {
      auto thanks = fixtures.at("GiftViewThanksResponse_zero");
      Put(thanks.payload, offset, count);
      Rejects([&] { (void)p::GiftViewThanksResponse::Decode(thanks); });
    }
  }
  auto truncated_count = fixtures.at("GiftReceivedListResponse_zero");
  Put(truncated_count.payload, 12, 1);
  Rejects([&] { (void)p::GiftReceivedListResponse::Decode(truncated_count); });
  auto long_string = fixtures.at("GiftSendRequest_zero");
  Put(long_string.payload, 0, 16385);
  Rejects([&] { (void)p::GiftSendRequest::Decode(long_string); });
  p::GiftReceivedListResponse received;
  received.records.resize(p::kMaxGiftElements);
  Roundtrip(received);
  received.records.emplace_back();
  Rejects([&] { (void)received.Encode(); });
  p::GiftSentListResponse sent;
  sent.records.resize(p::kMaxGiftElements);
  Roundtrip(sent);
  sent.records.emplace_back();
  Rejects([&] { (void)sent.Encode(); });
  p::GiftViewThanksResponse thanks;
  thanks.value_at0 =
      0xFFFFFFFF;  // The original bool conversion must not lose raw wire bits.
  thanks.extra.values_at4.resize(p::kMaxGiftElements, 0xFFFFFFFF);
  thanks.extra.values_at16.resize(p::kMaxGiftElements, 0x80000000);
  thanks.extra.values_at28.resize(p::kMaxGiftElements, 1);
  thanks.extra.bytes_at40.resize(p::kMaxGiftElements, 0xFF);
  thanks.extra.records_at52.resize(p::kMaxGiftElements);
  Roundtrip(thanks);
  thanks.extra.records_at52.emplace_back();
  Rejects([&] { (void)thanks.Encode(); });
  thanks.extra.records_at52.clear();
  thanks.extra.bytes_at40.push_back(0);
  Rejects([&] { (void)thanks.Encode(); });
  thanks.extra.bytes_at40.clear();
  thanks.extra.values_at4.push_back(0);
  Rejects([&] { (void)thanks.Encode(); });
  p::GiftSentListResponse large;
  large.records.resize(32);
  for (auto& record : large.records) {
    record.text_at28.assign(16384, u'A');
    record.text_at60.assign(16384, u'B');
    record.text_at84.assign(16384, u'C');
  }
  Rejects([&] { (void)large.Encode(); });
  p::GiftViewThanksResponse nested;
  nested.extra.records_at52.resize(256);
  for (auto& record : nested.extra.records_at52) record.values_at8.resize(1024);
  Rejects([&] { (void)nested.Encode(); });
  Rejects([&] {
    (void)p::GiftNewReceiveNotification::Decode(
        p::GiftReceiveRequest{1}.Encode());
  });
  Rejects([&] {
    (void)p::GiftReceivedListResponse::Decode(
        p::GiftViewThanksRequest{1}.Encode());
  });
  Rejects([&] {
    (void)p::GiftSendResponse::Decode(p::GiftDeleteReceivedRequest{1}.Encode());
  });
  std::cout << "Gift protocol: " << checks << " checks passed.\n";
  return checks;
}
