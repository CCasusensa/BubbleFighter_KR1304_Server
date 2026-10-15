#include "protocol/social/visitation_messages.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
void RequireId(const TcpPacket& packet, std::uint16_t id) {
  if (packet.message_id != id)
    throw std::invalid_argument("Unexpected visitation message id.");
}
void WriteName(PacketWriter& writer, const std::u16string& name) {
  if (name.size() > 32)
    throw std::length_error("Visitation name exceeds supported bounds.");
  writer.WriteUtf16(name);
}
std::u16string ReadName(PacketReader& reader) {
  const auto count = reader.ReadUInt32();
  if (count > 32)
    throw std::length_error("Visitation name exceeds supported bounds.");
  std::u16string result;
  result.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i)
    result.push_back(static_cast<char16_t>(reader.ReadUInt16()));
  return result;
}
}  // namespace
TcpPacket VisitationRequest::Encode() const {
  PacketWriter writer;
  WriteName(writer, target_name);
  return {kMessageId, writer.TakeBytes()};
}
VisitationRequest VisitationRequest::Decode(const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  VisitationRequest value{ReadName(reader)};
  reader.RequireEnd();
  return value;
}
TcpPacket DeclineVisitationRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(visitation_key);
  writer.WriteUInt32(value_at4);
  return {kMessageId, writer.TakeBytes()};
}
DeclineVisitationRequest DeclineVisitationRequest::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  DeclineVisitationRequest value{reader.ReadUInt32(), reader.ReadUInt32()};
  reader.RequireEnd();
  return value;
}
TcpPacket AcceptVisitationRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(visitation_key);
  return {kMessageId, writer.TakeBytes()};
}
AcceptVisitationRequest AcceptVisitationRequest::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  AcceptVisitationRequest value{reader.ReadUInt32()};
  reader.RequireEnd();
  return value;
}
TcpPacket VisitationFailedNotification::Encode() const {
  PacketWriter writer;
  WriteName(writer, target_name);
  writer.WriteUInt32(reason);
  return {kMessageId, writer.TakeBytes()};
}
VisitationFailedNotification VisitationFailedNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  VisitationFailedNotification value{ReadName(reader), reader.ReadUInt32()};
  reader.RequireEnd();
  return value;
}
TcpPacket AskedVisitationNotification::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(visitation_key);
  WriteName(writer, visitor_name);
  writer.WriteUInt32(value_at28);
  return {kMessageId, writer.TakeBytes()};
}
AskedVisitationNotification AskedVisitationNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  AskedVisitationNotification value{reader.ReadUInt32(), ReadName(reader),
                                    reader.ReadUInt32()};
  reader.RequireEnd();
  return value;
}
TcpPacket VisitationAcceptedNotification::Encode() const {
  PacketWriter writer;
  WriteName(writer, target_name);
  writer.WriteUInt32(address);
  writer.WriteUInt16(port);
  return {kMessageId, writer.TakeBytes()};
}
VisitationAcceptedNotification VisitationAcceptedNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  VisitationAcceptedNotification value{ReadName(reader), reader.ReadUInt32(),
                                       reader.ReadUInt16()};
  reader.RequireEnd();
  return value;
}
}  // namespace bubble_fighter::server::protocol
