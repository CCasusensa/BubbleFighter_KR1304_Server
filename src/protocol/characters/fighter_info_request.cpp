#include "protocol/characters/fighter_info_request.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
TcpPacket FighterInfoRequest::Encode() const {
  PacketWriter writer;
  for (const auto value : values) writer.WriteUInt32(value);
  return {kMessageId, writer.TakeBytes()};
}

FighterInfoRequest FighterInfoRequest::Decode(const TcpPacket& packet) {
  if (packet.message_id != kMessageId) {
    throw std::invalid_argument("Incorrect fighter info request message.");
  }
  PacketReader reader(packet.payload);
  FighterInfoRequest result;
  for (auto& value : result.values) value = reader.ReadUInt32();
  reader.RequireEnd();
  return result;
}
}  // namespace bubble_fighter::server::protocol
