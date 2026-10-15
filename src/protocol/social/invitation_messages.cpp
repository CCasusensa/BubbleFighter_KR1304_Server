#include "protocol/social/invitation_messages.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
void RequireId(const TcpPacket& packet, std::uint16_t id) {
  if (packet.message_id != id) {
    throw std::invalid_argument("Unexpected invitation message id.");
  }
}
// Resource/connection safety bound, separate from 90A330's 12-unit UI limit.
// Preserve all fields here; identity, consent and scene policy belong to the
// authenticated session and registry.
void WriteName(PacketWriter& writer, const std::u16string& name) {
  if (name.size() > 32) {
    throw std::length_error("Invitation name exceeds supported bounds.");
  }
  writer.WriteUtf16(name);
}
std::u16string ReadName(PacketReader& reader) {
  const auto count = reader.ReadUInt32();
  if (count > 32) {
    throw std::length_error("Invitation name exceeds supported bounds.");
  }
  std::u16string name;
  name.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    name.push_back(static_cast<char16_t>(reader.ReadUInt16()));
  }
  return name;
}
}  // namespace

TcpPacket InvitationRequest::Encode() const {
  PacketWriter writer;
  WriteName(writer, target_name);
  writer.WriteUInt32(value_at24);
  return {kMessageId, writer.TakeBytes()};
}
InvitationRequest InvitationRequest::Decode(const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  InvitationRequest value{ReadName(reader), reader.ReadUInt32()};
  reader.RequireEnd();
  return value;
}
TcpPacket DeclineInvitationRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(invitation_key);
  return {kMessageId, writer.TakeBytes()};
}
DeclineInvitationRequest DeclineInvitationRequest::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  DeclineInvitationRequest value{reader.ReadUInt32()};
  reader.RequireEnd();
  return value;
}
TcpPacket MoveToInviterRequest::Encode() const {
  PacketWriter writer;
  WriteName(writer, inviter_name);
  writer.WriteUInt32(address);
  writer.WriteUInt16(port);
  writer.WriteUInt8(byte_at32);
  return {kMessageId, writer.TakeBytes()};
}
MoveToInviterRequest MoveToInviterRequest::Decode(const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  MoveToInviterRequest value{ReadName(reader), reader.ReadUInt32(),
                             reader.ReadUInt16(), reader.ReadUInt8()};
  reader.RequireEnd();
  return value;
}
TcpPacket InvitationFailedNotification::Encode() const {
  PacketWriter writer;
  WriteName(writer, target_name);
  writer.WriteUInt32(reason);
  return {kMessageId, writer.TakeBytes()};
}
InvitationFailedNotification InvitationFailedNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  InvitationFailedNotification value{ReadName(reader), reader.ReadUInt32()};
  reader.RequireEnd();
  return value;
}
TcpPacket InvitationDeliveredNotification::Encode() const {
  PacketWriter writer;
  WriteName(writer, target_name);
  return {kMessageId, writer.TakeBytes()};
}
InvitationDeliveredNotification InvitationDeliveredNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  InvitationDeliveredNotification value{ReadName(reader)};
  reader.RequireEnd();
  return value;
}
TcpPacket AskedInvitationNotification::Encode() const {
  PacketWriter writer;
  WriteName(writer, inviter_name);
  writer.WriteUInt32(invitation_key);
  writer.WriteUInt32(address);
  writer.WriteUInt16(port);
  writer.WriteUInt32(value_at36);
  writer.WriteUInt32(value_at40);
  return {kMessageId, writer.TakeBytes()};
}
AskedInvitationNotification AskedInvitationNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  AskedInvitationNotification value{ReadName(reader),    reader.ReadUInt32(),
                                    reader.ReadUInt32(), reader.ReadUInt16(),
                                    reader.ReadUInt32(), reader.ReadUInt32()};
  reader.RequireEnd();
  return value;
}
TcpPacket MoveToInviterFailedNotification::Encode() const {
  PacketWriter writer;
  WriteName(writer, inviter_name);
  writer.WriteUInt32(reason);
  return {kMessageId, writer.TakeBytes()};
}
MoveToInviterFailedNotification MoveToInviterFailedNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  MoveToInviterFailedNotification value{ReadName(reader), reader.ReadUInt32()};
  reader.RequireEnd();
  return value;
}
}  // namespace bubble_fighter::server::protocol
