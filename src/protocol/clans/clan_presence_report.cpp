#include "protocol/clans/clan_presence_report.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
ClanPacket ClanPresenceReport::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(reported_level);
  writer.WriteUInt32(reported_scene);
  return {kMessageId, writer.TakeBytes()};
}
ClanPresenceReport ClanPresenceReport::Decode(const ClanPacket& packet) {
  if (packet.message_id != kMessageId || packet.payload.size() != 8)
    throw std::invalid_argument("Invalid clan presence report.");
  PacketReader reader(packet.payload);
  return {reader.ReadUInt32(), reader.ReadUInt32()};
}
}  // namespace bubble_fighter::server::protocol
