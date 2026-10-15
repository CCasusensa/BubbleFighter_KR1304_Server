#include "protocol/clans/clan_disband_messages.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
void Check(const ClanPacket& packet, std::uint16_t id, std::size_t size) {
  if (packet.message_id != id || packet.payload.size() != size)
    throw std::invalid_argument("Invalid clan disband packet.");
}
}  // namespace
ClanPacket ClanDisbandRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(action);
  return {kMessageId, writer.TakeBytes()};
}
ClanDisbandRequest ClanDisbandRequest::Decode(const ClanPacket& packet) {
  Check(packet, kMessageId, 4);
  return {PacketReader(packet.payload).ReadUInt32()};
}
ClanPacket ClanDisbandResponse::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(action);
  return {kMessageId, writer.TakeBytes()};
}
ClanDisbandResponse ClanDisbandResponse::Decode(const ClanPacket& packet) {
  Check(packet, kMessageId, 8);
  PacketReader reader(packet.payload);
  return {reader.ReadUInt32(), reader.ReadUInt32()};
}
ClanPacket ClanDisbandNotification::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(action);
  return {kMessageId, writer.TakeBytes()};
}
ClanDisbandNotification ClanDisbandNotification::Decode(
    const ClanPacket& packet) {
  Check(packet, kMessageId, 8);
  PacketReader reader(packet.payload);
  return {reader.ReadUInt32(), reader.ReadUInt32()};
}
}  // namespace bubble_fighter::server::protocol
