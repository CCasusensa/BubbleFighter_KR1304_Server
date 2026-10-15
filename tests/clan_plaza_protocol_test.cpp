#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "protocol/clans/clan_plaza_request.h"
#include "protocol/codec/packet_writer.h"
#include "protocol/scenes/goto_room_notification.h"

namespace {
namespace p = bubble_fighter::server::protocol;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Clan plaza protocol line " +
                             std::to_string(at.line()));
}
template <class F>
void Rejects(F f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception&) {
    failed = true;
  }
  Check(failed);
}
void Same(const p::TcpPacket& a, const p::TcpPacket& b) {
  Check(a.message_id == b.message_id && a.payload == b.payload);
}
struct RequestCase {
  const char* label;
  std::uint64_t id;
  std::u16string name;
};
const std::array<RequestCase, 11> kRequests{{
    {"request_empty", 0, u""},
    {"request_clan", 9000, u"클랜"},
    {"request_training", 99, u"훈련소"},
    {"request_inline7", 7, u"Clan007"},
    {"request_heap8", 8, u"Clan0008"},
    {"request_long", 9001, u"Clan_Plaza_2028"},
    {"request_max_signed", 0x7FFFFFFF, u"Max"},
    {"request_negative", 0xFFFFFFFFFFFFFFFFULL, u"Minus"},
    {"request_surrogates", 9002, u"A\U0001F30AB"},
    {"request_embedded_zero", 9003, std::u16string(u"A\0B", 3)},
    {"request_wide_codec_only", 0x123456789ABCDEF0ULL, u"Wide"},
}};
struct SceneCase {
  const char* label;
  std::uint32_t type;
  std::uint64_t wide;
  std::u16string name;
};
const std::array<SceneCase, 6> kScenes{{
    {"scene_ordinary", 3, 9000, u"Private"},
    {"scene_clan_inline", 6, 9000, u"클랜"},
    {"scene_clan_heap", 6, 9001, u"Clan0008"},
    {"scene_clan_empty", 6, 0, u""},
    {"scene_training", 6, 99, u"훈련소"},
    {"scene_menu", 1, 9000, u"Private"},
}};
}  // namespace

int RunClanPlazaProtocolTests(const std::filesystem::path& directory) {
  checks = 0;
  std::ifstream input(directory / "clan_plaza_protocol_packets.txt");
  Check(input.good());
  std::string label, hex;
  std::uint16_t mid = 0;
  std::size_t requests = 0, scenes = 0;
  while (input >> label >> mid >> hex) {
    p::TcpPacket packet{mid, {}};
    Check(hex.size() % 2 == 0);
    for (std::size_t i = 0; i < hex.size(); i += 2)
      packet.payload.push_back(
          static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    if (mid == 2028) {
      Check(requests < kRequests.size());
      const auto& expected = kRequests.at(requests++);
      Check(label == expected.label);
      const p::ClanPlazaRequest request{expected.id, expected.name};
      Check(p::ClanPlazaRequest::Decode(packet) == request);
      Same(request.Encode(), packet);
    } else {
      Check(mid == 117 && scenes < kScenes.size());
      const auto& expected = kScenes.at(scenes++);
      Check(label == expected.label);
      p::GotoRoomNotification message;
      message.descriptor.short_at0 = 17;
      message.descriptor.text_at8 = u"Common";
      message.descriptor.shorts_at32 = {0, 0, 16};
      message.descriptor.session_type = expected.type;
      message.descriptor.wide_at80 = expected.wide;
      message.descriptor.text_at88 = expected.name;
      Check(p::GotoRoomNotification::Decode(packet) == message);
      Same(message.Encode(), packet);
    }
    const auto decode = [&](const p::TcpPacket& bad) {
      if (mid == 2028)
        p::ClanPlazaRequest::Decode(bad);
      else
        p::GotoRoomNotification::Decode(bad);
    };
    for (std::size_t length = 0; length < packet.payload.size(); ++length) {
      auto bad = packet;
      bad.payload.resize(length);
      Rejects([&] { decode(bad); });
    }
    auto bad = packet;
    ++bad.message_id;
    Rejects([&] { decode(bad); });
    bad = packet;
    bad.payload.push_back(0);
    Rejects([&] { decode(bad); });
  }
  Check(requests == kRequests.size() && scenes == kScenes.size());
  p::ClanPlazaRequest limit{
      1, std::u16string(p::PacketWriter::kMaxStringCodeUnits, u'界')};
  Check(p::ClanPlazaRequest::Decode(limit.Encode()) == limit);
  limit.reported_clan_name.push_back(u'界');
  Rejects([&] { limit.Encode(); });
  for (const std::uint32_t length : {0x4001U, 0x80000000U, 0xFFFFFFFFU}) {
    p::PacketWriter writer;
    writer.WriteUInt64(9000);
    writer.WriteUInt32(length);
    const p::TcpPacket bad{2028, writer.TakeBytes()};
    Rejects([&] { p::ClanPlazaRequest::Decode(bad); });
  }
  std::cout << "Clan plaza protocol: " << checks << " checks passed.\n";
  return checks;
}
