#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "protocol/codec/packet_writer.h"
#include "protocol/social/companion_mailbox_messages.h"

namespace {
namespace p = bubble_fighter::server::protocol;
using Request = p::CompanionMailboxRequest;
using Batch = p::CompanionMailboxNotification;
using Action = p::CompanionMailboxAction;
using Status = p::CompanionMailboxStatus;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Mailbox protocol line " +
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
template <class M>
void Framing(const p::TcpPacket& packet) {
  Check(M::Decode(packet).Encode().payload == packet.payload);
  for (std::size_t n = 0; n < packet.payload.size(); ++n) {
    auto bad = packet;
    bad.payload.resize(n);
    Rejects([&] { (void)M::Decode(bad); });
  }
  auto bad = packet;
  bad.payload.push_back(0);
  Rejects([&] { (void)M::Decode(bad); });
  bad = packet;
  bad.message_id = 65010;
  Rejects([&] { (void)M::Decode(bad); });
  bad = packet;
  bad.payload[0] = 2;
  Rejects([&] { (void)M::Decode(bad); });
}
p::TcpPacket Raw(const Batch& batch) {
  p::PacketWriter writer;
  writer.WriteUInt32(1);
  writer.WriteUInt32(static_cast<std::uint32_t>(batch.status));
  writer.WriteUInt32(static_cast<std::uint32_t>(batch.messages.size()));
  for (const auto& m : batch.messages) {
    writer.WriteUInt64(m.message_id);
    writer.WriteUInt32(m.sender_id);
    writer.WriteUInt64(m.sent_at_unix_seconds);
    writer.WriteUtf16(m.sender_name);
    writer.WriteUtf16(m.text);
  }
  return {65017, writer.TakeBytes()};
}
}  // namespace

int RunOfflineMessageProtocolTests(const std::filesystem::path& directory) {
  checks = 0;
  const std::array<Request, 3> requests{
      {{},
       {Action::kAcknowledge, 42},
       {Action::kAcknowledge, 0xfedcba9876543210}}};
  const std::array<Batch, 5> batches{
      {{},
       {Status::kOk, {{42, 7, 1792000000, u"Alice", u"Hello"}}},
       {Status::kOk,
        {{43, 8, 1792000001, u"친구", u"留言\U0001f600"},
         {0x100000001, 7, 1792000002, u"Alice", u"Again"}}},
       {Status::kUnavailable, {}},
       {Status::kRateLimited, {}}}};
  std::ifstream file(directory / "offline_message_packets.txt");
  Check(file.good());
  std::string label, hex;
  std::uint16_t id = 0;
  std::size_t ri = 0, bi = 0;
  while (file >> label >> id >> hex) {
    p::TcpPacket packet{id, {}};
    Check(hex.size() % 2 == 0);
    for (std::size_t i = 0; i < hex.size(); i += 2)
      packet.payload.push_back(
          static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    if (id == 65016) {
      Check(ri < requests.size());
      const auto& expected = requests.at(ri++);
      Check(expected.Encode().payload == packet.payload);
      const auto actual = Request::Decode(packet);
      Check(actual.action == expected.action &&
            actual.message_id == expected.message_id);
      Framing<Request>(packet);
    } else {
      Check(id == 65017 && bi < batches.size());
      const auto& expected = batches.at(bi++);
      Check(expected.Encode().payload == packet.payload);
      const auto actual = Batch::Decode(packet);
      Check(actual.status == expected.status &&
            actual.messages == expected.messages);
      Framing<Batch>(packet);
    }
  }
  Check(ri == 3 && bi == 5);
  for (const auto& invalid :
       std::array<Request, 4>{{{Action::kFetch, 1},
                               {Action::kAcknowledge, 0},
                               {static_cast<Action>(2), 0},
                               {static_cast<Action>(255), UINT64_MAX}}}) {
    Rejects([&] { (void)invalid.Encode(); });
    p::PacketWriter writer;
    writer.WriteUInt32(1);
    writer.WriteUInt8(static_cast<std::uint8_t>(invalid.action));
    writer.WriteUInt64(invalid.message_id);
    const p::TcpPacket packet{65016, writer.TakeBytes()};
    Rejects([&] { (void)Request::Decode(packet); });
  }
  for (int n = 0; n < 13; ++n) {
    auto bad = batches[2];
    switch (n) {
      case 0:
        bad.messages[0].message_id = 0;
        break;
      case 1:
        bad.messages[1].message_id = 43;
        break;
      case 2:
        bad.messages[1].message_id = 42;
        break;
      case 3:
        bad.messages[0].sender_id = 0;
        break;
      case 4:
        bad.messages[0].sent_at_unix_seconds = 0;
        break;
      case 5:
        bad.messages[0].sent_at_unix_seconds = Batch::kMaximumTimestamp + 1;
        break;
      case 6:
        bad.messages[0].sender_name.clear();
        break;
      case 7:
        bad.messages[0].text.clear();
        break;
      case 8:
        bad.messages[0].sender_name = std::u16string(33, u'N');
        break;
      case 9:
        bad.messages[0].text = std::u16string(257, u'T');
        break;
      case 10:
        bad.status = Status::kUnavailable;
        break;
      case 11:
        bad.status = Status::kRateLimited;
        break;
      case 12:
        bad.status = static_cast<Status>(3);
        break;
    }
    Rejects([&] { (void)bad.Encode(); });
    Rejects([&] { (void)Batch::Decode(Raw(bad)); });
  }
  for (const auto& invalid :
       std::vector<std::u16string>{std::u16string(u"A\0B", 3),
                                   u"A\rB",
                                   u"A\nB",
                                   u"A\tB",
                                   std::u16string(1, 0x7f),
                                   std::u16string(1, 0xd800),
                                   std::u16string(1, 0xdc00),
                                   {0xd800, u'A'},
                                   {0xdc00, 0xd800}}) {
    for (bool name : {false, true}) {
      auto bad = batches[1];
      (name ? bad.messages[0].sender_name : bad.messages[0].text) = invalid;
      Rejects([&] { (void)bad.Encode(); });
      Rejects([&] { (void)Batch::Decode(Raw(bad)); });
    }
  }
  Batch maximum;
  for (std::uint64_t i = 1; i <= 20; ++i)
    maximum.messages.push_back({i, UINT32_MAX, Batch::kMaximumTimestamp,
                                std::u16string(30, u'N') + u"\U0001f600",
                                std::u16string(254, u'T') + u"\U0001f600"});
  Check(Batch::Decode(maximum.Encode()).messages == maximum.messages);
  maximum.messages.push_back({21, 1, 1, u"A", u"B"});
  Rejects([&] { (void)maximum.Encode(); });
  Rejects([&] { (void)Batch::Decode(Raw(maximum)); });
  for (auto count : {21U, UINT32_MAX}) {
    p::PacketWriter w;
    w.WriteUInt32(1);
    w.WriteUInt32(0);
    w.WriteUInt32(count);
    Rejects([&] { (void)Batch::Decode({65017, w.TakeBytes()}); });
  }
  // Independent fixed offsets: name length32, text length46 in fixture 'one'.
  for (std::size_t offset : {32U, 46U}) {
    auto bad = batches[1].Encode();
    for (std::size_t j = 0; j < 4; ++j) bad.payload[offset + j] = 0xff;
    Rejects([&] { (void)Batch::Decode(bad); });
  }
  auto wide = batches[1];
  wide.messages[0].message_id = UINT64_MAX;
  Check(Batch::Decode(wide.Encode()).messages[0].message_id == UINT64_MAX);
  std::cout << "Offline message protocol: " << checks << " checks passed.\n";
  return checks;
}
