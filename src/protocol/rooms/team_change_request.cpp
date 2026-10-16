#include "protocol/rooms/team_change_request.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
void AppendRest(PacketReader& reader, std::vector<std::uint8_t>& out) {
  while (reader.RemainingBytes() > 0) out.push_back(reader.ReadUInt8());
}
}  // namespace

TcpPacket TeamChangeRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt8(value_at0 ? kPresentMarker : kAbsentMarker);
  if (value_at0) {
    writer.WriteUInt32(*value_at0);
  }
  for (const auto byte : rest) writer.WriteUInt8(byte);
  return {kMessageId, writer.TakeBytes()};
}

TeamChangeRequest TeamChangeRequest::Decode(const TcpPacket& packet) {
  if (packet.message_id != kMessageId) {
    throw std::invalid_argument("Unexpected team-change request id.");
  }
  PacketReader reader(packet.payload);
  TeamChangeRequest request;
  // 0x00ADEF30 writes absent/absent, the other two writers present values.
  const auto first = reader.ReadUInt8();
  if (first == kPresentMarker) {
    request.value_at0 = reader.ReadUInt32();
  } else if (first != kAbsentMarker) {
    throw std::runtime_error("Invalid team-change optional marker.");
  }
  if (reader.RemainingBytes() == 0) {
    return request;
  }
  // 0x00ADEF30 follows the first field with its second absent marker,
  // 0x004B3AC0 with an uninterpreted structure and 0x004A1EB0 with a
  // present group: all of it is writer-specific and kept verbatim.
  AppendRest(reader, request.rest);
  return request;
}
}  // namespace bubble_fighter::server::protocol
