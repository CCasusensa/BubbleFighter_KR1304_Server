#include "protocol/scenes/channel_transfer.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
void RequireMessage(const TcpPacket& packet, std::uint16_t id) {
  if (packet.message_id != id)
    throw std::invalid_argument("Unexpected channel transfer message id.");
}
void RequireEmpty(const TcpPacket& packet, std::uint16_t id) {
  RequireMessage(packet, id);
  PacketReader(packet.payload).RequireEnd();
}
}  // namespace

TcpPacket MoveChannelRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(channel_category);
  return {kMessageId, writer.TakeBytes()};
}
MoveChannelRequest MoveChannelRequest::Decode(const TcpPacket& packet) {
  RequireMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  MoveChannelRequest message{reader.ReadUInt32()};
  reader.RequireEnd();
  return message;
}
TcpPacket MoveIntoRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(character_id);
  writer.WriteUInt32(unknown_ui_value);
  return {kMessageId, writer.TakeBytes()};
}
MoveIntoRequest MoveIntoRequest::Decode(const TcpPacket& packet) {
  RequireMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  MoveIntoRequest message{reader.ReadUInt32(), reader.ReadUInt32()};
  reader.RequireEnd();
  return message;
}
TcpPacket MoveChannelResponse::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(result);
  for (const auto octet : ipv4_address) writer.WriteUInt8(octet);
  writer.WriteUInt16(port);
  writer.WriteUInt32(channel_category);
  return {kMessageId, writer.TakeBytes()};
}
MoveChannelResponse MoveChannelResponse::Decode(const TcpPacket& packet) {
  RequireMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  MoveChannelResponse message;
  message.result = reader.ReadUInt32();
  for (auto& octet : message.ipv4_address) octet = reader.ReadUInt8();
  message.port = reader.ReadUInt16();
  message.channel_category = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}
TcpPacket ChannelSwitchAcknowledgementRequest::Encode() const {
  return {kMessageId, {}};
}
ChannelSwitchAcknowledgementRequest ChannelSwitchAcknowledgementRequest::Decode(
    const TcpPacket& packet) {
  RequireEmpty(packet, kMessageId);
  return {};
}
TcpPacket ChannelDisconnectingNotification::Encode() const {
  return {kMessageId, {}};
}
ChannelDisconnectingNotification ChannelDisconnectingNotification::Decode(
    const TcpPacket& packet) {
  RequireEmpty(packet, kMessageId);
  return {};
}
TcpPacket ChannelDisconnectNotification::Encode() const {
  return {kMessageId, {}};
}
ChannelDisconnectNotification ChannelDisconnectNotification::Decode(
    const TcpPacket& packet) {
  RequireEmpty(packet, kMessageId);
  return {};
}
TcpPacket FirstChannelMoveNotification::Encode() const {
  return {kMessageId, {}};
}
FirstChannelMoveNotification FirstChannelMoveNotification::Decode(
    const TcpPacket& packet) {
  RequireEmpty(packet, kMessageId);
  return {};
}
}  // namespace bubble_fighter::server::protocol
