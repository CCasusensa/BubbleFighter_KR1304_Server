#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "protocol/codec/packet_writer.h"
#include "protocol/shop/cash_refund_request.h"
#include "protocol/shop/cash_refund_response.h"

namespace {
namespace p = bubble_fighter::server::protocol;
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value) {
    throw std::runtime_error("Cash refund protocol line " +
                             std::to_string(at.line()));
  }
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
struct TextCase {
  const char* label;
  std::u16string text;
};
const std::array<TextCase, 9> kTexts{{
    {"empty", {}},
    {"one", u"R"},
    {"inline7", u"ABC1234"},
    {"heap8", u"ABC12345"},
    {"unicode", u"退款\U0001F30A"},
    {"nul", std::u16string(u"A\0B", 3)},
    {"surrogate", std::u16string(1, static_cast<char16_t>(0xD800))},
    {"short_limit", std::u16string(1023, u'X')},
    {"heap_limit", std::u16string(1024, u'X')},
}};
constexpr std::array<std::uint32_t, 5> kSlots{0, 1, 0x10203040, 0x80000000,
                                              0xFFFFFFFF};
}  // namespace

int RunCashRefundProtocolTests(const std::filesystem::path& directory) {
  checks = 0;
  std::ifstream input(directory / "cash_refund_protocol_packets.txt");
  Check(input.good());
  std::string label, hex;
  std::uint16_t mid = 0;
  std::size_t requests = 0, responses = 0;
  while (input >> label >> mid >> hex) {
    p::TcpPacket packet{mid, {}};
    Check(hex.size() % 2 == 0);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      packet.payload.push_back(
          static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    if (mid == 817) {
      Check(requests < kTexts.size());
      const auto& row = kTexts.at(requests);
      const p::CashRefundRequest expected{row.text,
                                          kSlots[requests % kSlots.size()]};
      ++requests;
      Check(label == std::string("request_") + row.label);
      Check(p::CashRefundRequest::Decode(packet) == expected);
      Check(expected.Encode().message_id == 817);
      Check(expected.Encode().payload == packet.payload);
    } else {
      Check(mid == 824 && responses < kTexts.size());
      const auto& row = kTexts.at(responses);
      const p::CashRefundResponse expected{
          0xFEDCBA98,
          responses < 8 ? static_cast<std::uint32_t>(responses) : 0xFFFFFFFF,
          row.text};
      ++responses;
      Check(label == std::string("response_") + row.label);
      Check(p::CashRefundResponse::Decode(packet) == expected);
      Check(expected.Encode().message_id == 824);
      Check(expected.Encode().payload == packet.payload);
    }
    const auto decode = [&](const p::TcpPacket& bad) {
      if (mid == 817) {
        (void)p::CashRefundRequest::Decode(bad);
      } else {
        (void)p::CashRefundResponse::Decode(bad);
      }
    };
    for (std::size_t length = 0; length < packet.payload.size(); ++length) {
      auto bad = packet;
      bad.payload.resize(length);
      Rejects([&] { decode(bad); });
    }
    auto bad = packet;
    bad.payload.push_back(0);
    Rejects([&] { decode(bad); });
    bad = packet;
    bad.message_id = mid == 817 ? 829 : 835;
    Rejects([&] { decode(bad); });
    bad.message_id = mid == 817 ? 824 : 817;
    Rejects([&] { decode(bad); });
  }
  Check(requests == kTexts.size() && responses == kTexts.size());
  p::CashRefundRequest request{
      std::u16string(p::PacketWriter::kMaxStringCodeUnits, u'界'), 0xFFFFFFFF};
  p::CashRefundResponse response{0xFFFFFFFF, 0xFFFFFFFF, request.text_at0};
  Check(p::CashRefundRequest::Decode(request.Encode()) == request);
  Check(p::CashRefundResponse::Decode(response.Encode()) == response);
  Check(request.Encode().payload.size() == 8 + 16384 * 2);
  Check(response.Encode().payload.size() == 12 + 16384 * 2);
  request.text_at0.push_back(u'界');
  response.text_at8.push_back(u'界');
  Rejects([&] { (void)request.Encode(); });
  Rejects([&] { (void)response.Encode(); });
  for (const auto length : {16384U, 16385U, 0x80000000U, 0xFFFFFFFFU}) {
    for (const auto message : {817, 824}) {
      p::PacketWriter writer;
      if (message == 824) {
        writer.WriteUInt32(1);
        writer.WriteUInt32(1);
      }
      writer.WriteUInt32(length);
      const p::TcpPacket bad{static_cast<std::uint16_t>(message),
                             writer.TakeBytes()};
      if (message == 817) {
        Rejects([&] { (void)p::CashRefundRequest::Decode(bad); });
      } else {
        Rejects([&] { (void)p::CashRefundResponse::Decode(bad); });
      }
    }
  }
  Check(p::CashRefundResponse::kSuccess == 1);
  Check(p::CashRefundResponse::kExpired == 4);
  std::cout << "Cash refund protocol: " << checks << " checks passed.\n";
  return checks;
}
