#include "protocol/parties/party_messages.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
void RequireId(const TcpPacket& packet, std::uint16_t id) {
  if (packet.message_id != id)
    throw std::invalid_argument("Unexpected party operation message id.");
}
void WriteName(PacketWriter& writer, const std::u16string& name) {
  if (name.size() > 32)
    throw std::length_error("Party operation name exceeds supported bounds.");
  writer.WriteUtf16(name);
}
std::u16string ReadName(PacketReader& reader) {
  const auto count = reader.ReadUInt32();
  if (count > 32)
    throw std::length_error("Party operation name exceeds supported bounds.");
  std::u16string value;
  value.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i)
    value.push_back(static_cast<char16_t>(reader.ReadUInt16()));
  return value;
}
}  // namespace
TcpPacket LeavePartyRequest::Encode() const {
  PacketWriter writer;
  return {kMessageId, writer.TakeBytes()};
}
LeavePartyRequest LeavePartyRequest::Decode(const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  LeavePartyRequest value{};
  reader.RequireEnd();
  return value;
}
TcpPacket KickPartyRequest::Encode() const {
  PacketWriter writer;
  WriteName(writer, target_name);
  return {kMessageId, writer.TakeBytes()};
}
KickPartyRequest KickPartyRequest::Decode(const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  KickPartyRequest value{ReadName(reader)};
  reader.RequireEnd();
  return value;
}
TcpPacket ChangePartyLeaderRequest::Encode() const {
  PacketWriter writer;
  WriteName(writer, target_name);
  return {kMessageId, writer.TakeBytes()};
}
ChangePartyLeaderRequest ChangePartyLeaderRequest::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  ChangePartyLeaderRequest value{ReadName(reader)};
  reader.RequireEnd();
  return value;
}
TcpPacket PartyLeaderLeftNotification::Encode() const {
  PacketWriter writer;
  WriteName(writer, previous_leader_name);
  WriteName(writer, new_leader_name);
  return {kMessageId, writer.TakeBytes()};
}
PartyLeaderLeftNotification PartyLeaderLeftNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  PartyLeaderLeftNotification value{ReadName(reader), ReadName(reader)};
  reader.RequireEnd();
  return value;
}
TcpPacket PartyLeaveNotification::Encode() const {
  PacketWriter writer;
  return {kMessageId, writer.TakeBytes()};
}
PartyLeaveNotification PartyLeaveNotification::Decode(const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  PartyLeaveNotification value{};
  reader.RequireEnd();
  return value;
}
TcpPacket PartyMemberLeftNotification::Encode() const {
  PacketWriter writer;
  WriteName(writer, member_name);
  return {kMessageId, writer.TakeBytes()};
}
PartyMemberLeftNotification PartyMemberLeftNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  PartyMemberLeftNotification value{ReadName(reader)};
  reader.RequireEnd();
  return value;
}
TcpPacket PartyMemberJoinedNotification::Encode() const {
  PacketWriter writer;
  WriteName(writer, member_name);
  return {kMessageId, writer.TakeBytes()};
}
PartyMemberJoinedNotification PartyMemberJoinedNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  PartyMemberJoinedNotification value{ReadName(reader)};
  reader.RequireEnd();
  return value;
}
TcpPacket KickPartyResponse::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(reason);
  WriteName(writer, target_name);
  return {kMessageId, writer.TakeBytes()};
}
KickPartyResponse KickPartyResponse::Decode(const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  KickPartyResponse value{reader.ReadUInt32(), ReadName(reader)};
  reader.RequireEnd();
  return value;
}
TcpPacket ChangePartyLeaderResponse::Encode() const {
  PacketWriter writer;
  WriteName(writer, new_leader_name);
  WriteName(writer, previous_leader_name);
  writer.WriteUInt32(reason);
  return {kMessageId, writer.TakeBytes()};
}
ChangePartyLeaderResponse ChangePartyLeaderResponse::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  ChangePartyLeaderResponse value{ReadName(reader), ReadName(reader),
                                  reader.ReadUInt32()};
  reader.RequireEnd();
  return value;
}
TcpPacket KickedFromPartyNotification::Encode() const {
  PacketWriter writer;
  WriteName(writer, kicker_name);
  return {kMessageId, writer.TakeBytes()};
}
KickedFromPartyNotification KickedFromPartyNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  KickedFromPartyNotification value{ReadName(reader)};
  reader.RequireEnd();
  return value;
}
TcpPacket PartyKickNotification::Encode() const {
  PacketWriter writer;
  WriteName(writer, kicker_name);
  WriteName(writer, target_name);
  return {kMessageId, writer.TakeBytes()};
}
PartyKickNotification PartyKickNotification::Decode(const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader reader(packet.payload);
  PartyKickNotification value{ReadName(reader), ReadName(reader)};
  reader.RequireEnd();
  return value;
}
}  // namespace bubble_fighter::server::protocol
