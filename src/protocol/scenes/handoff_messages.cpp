#include "protocol/scenes/handoff_messages.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
PacketReader Read(const TcpPacket& packet, std::uint16_t id) {
  if (packet.message_id != id)
    throw std::invalid_argument("Unexpected handoff companion message.");
  PacketReader reader(packet.payload);
  if (reader.ReadUInt32() != 1)
    throw std::invalid_argument("Unsupported handoff companion version.");
  return reader;
}
}  // namespace
TcpPacket HandoffCapabilityRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(1);
  return {kMessageId, writer.TakeBytes()};
}
HandoffCapabilityRequest HandoffCapabilityRequest::Decode(
    const TcpPacket& packet) {
  Read(packet, kMessageId).RequireEnd();
  return {};
}
TcpPacket HandoffGrantNotification::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(1);
  writer.WriteUInt32(character_id);
  writer.WriteUInt32(channel_category);
  for (const auto byte : secret) writer.WriteUInt8(byte);
  return {kMessageId, writer.TakeBytes()};
}
HandoffGrantNotification HandoffGrantNotification::Decode(
    const TcpPacket& packet) {
  auto reader = Read(packet, kMessageId);
  HandoffGrantNotification value;
  value.character_id = reader.ReadUInt32();
  value.channel_category = reader.ReadUInt32();
  for (auto& byte : value.secret) byte = reader.ReadUInt8();
  reader.RequireEnd();
  return value;
}
TcpPacket HandoffProofRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(1);
  writer.WriteUInt32(character_id);
  for (const auto byte : secret) writer.WriteUInt8(byte);
  return {kMessageId, writer.TakeBytes()};
}
HandoffProofRequest HandoffProofRequest::Decode(const TcpPacket& packet) {
  auto reader = Read(packet, kMessageId);
  HandoffProofRequest value;
  value.character_id = reader.ReadUInt32();
  for (auto& byte : value.secret) byte = reader.ReadUInt8();
  reader.RequireEnd();
  return value;
}
TcpPacket HandoffCancelledNotification::Encode() const {
  return {kMessageId, {1, 0, 0, 0}};
}
HandoffCancelledNotification HandoffCancelledNotification::Decode(
    const TcpPacket& packet) {
  Read(packet, kMessageId).RequireEnd();
  return {};
}
}  // namespace bubble_fighter::server::protocol
