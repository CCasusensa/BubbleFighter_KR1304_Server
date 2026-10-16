#include "protocol/characters/fighter_info_show_notification.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
TcpPacket FighterInfoShowNotification::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(value_at0);
  target.Write(writer);
  self.Write(writer);
  for (const auto value : values_at_end) writer.WriteUInt32(value);
  return {kMessageId, writer.TakeBytes()};
}

FighterInfoShowNotification FighterInfoShowNotification::Decode(
    const TcpPacket& packet) {
  if (packet.message_id != kMessageId) {
    throw std::invalid_argument("Incorrect fighter info show message.");
  }
  PacketReader reader(packet.payload);
  FighterInfoShowNotification result;
  result.value_at0 = reader.ReadUInt32();
  result.target = FighterInfo::Read(reader);
  result.self = FighterInfo::Read(reader);
  for (auto& value : result.values_at_end) value = reader.ReadUInt32();
  reader.RequireEnd();
  return result;
}
}  // namespace bubble_fighter::server::protocol
