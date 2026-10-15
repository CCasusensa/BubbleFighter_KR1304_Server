#include "protocol/clans/clan_membership_messages.h"

#include <initializer_list>
#include <stdexcept>
#include <string_view>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
constexpr auto kLimit = ClanPacketCodec::kMaxPayloadSize;
void CheckMessage(const ClanPacket& packet, std::uint16_t expected) {
  if (packet.message_id != expected)
    throw std::invalid_argument("Unexpected clan message id.");
  if (packet.payload.size() > kLimit)
    throw std::invalid_argument("Clan message exceeds native frame capacity.");
}
void CheckSize(std::size_t scalar_bytes,
               std::initializer_list<std::u16string_view> strings) {
  auto remaining = kLimit - scalar_bytes;
  for (const auto text : strings) {
    if (text.size() > (kLimit - 2) / 2 ||
        text.find(u'\0') != std::u16string_view::npos)
      throw std::invalid_argument(
          "Clan string cannot be represented as UTF-16Z.");
    const auto size = 2 + 2 * text.size();
    if (size > remaining)
      throw std::invalid_argument(
          "Clan message exceeds native frame capacity.");
    remaining -= size;
  }
}
void WriteText(PacketWriter& writer, std::u16string_view text) {
  for (const auto unit : text)
    writer.WriteUInt16(static_cast<std::uint16_t>(unit));
  writer.WriteUInt16(0);
}
std::u16string ReadText(PacketReader& reader) {
  std::u16string text;
  for (;;) {
    const auto unit = reader.ReadUInt16();
    if (!unit) return text;
    text.push_back(static_cast<char16_t>(unit));
  }
}
}  // namespace

ClanPacket ClanJoinRequest::Encode() const {
  CheckSize(4, {clan_name});
  PacketWriter writer;
  writer.WriteUInt32(clan_id);
  WriteText(writer, clan_name);
  return {kMessageId, writer.TakeBytes()};
}
ClanJoinRequest ClanJoinRequest::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanJoinRequest message;
  message.clan_id = reader.ReadUInt32();
  message.clan_name = ReadText(reader);
  reader.RequireEnd();
  return message;
}

ClanPacket ClanAutoJoinRequest::Encode() const {
  CheckSize(4, {clan_name, character_name});
  PacketWriter writer;
  writer.WriteUInt32(clan_id);
  WriteText(writer, clan_name);
  WriteText(writer, character_name);
  return {kMessageId, writer.TakeBytes()};
}
ClanAutoJoinRequest ClanAutoJoinRequest::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanAutoJoinRequest message;
  message.clan_id = reader.ReadUInt32();
  message.clan_name = ReadText(reader);
  message.character_name = ReadText(reader);
  reader.RequireEnd();
  return message;
}

ClanPacket ClanJoinResponse::Encode() const {
  CheckSize(12, {clan_name});
  PacketWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(membership_status);
  writer.WriteUInt32(clan_id);
  WriteText(writer, clan_name);
  return {kMessageId, writer.TakeBytes()};
}
ClanJoinResponse ClanJoinResponse::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanJoinResponse message;
  message.result = reader.ReadUInt32();
  message.membership_status = reader.ReadUInt32();
  message.clan_id = reader.ReadUInt32();
  message.clan_name = ReadText(reader);
  reader.RequireEnd();
  return message;
}

ClanPacket ClanApplicationChangedNotification::Encode() const {
  return {kMessageId, {}};
}
ClanApplicationChangedNotification ClanApplicationChangedNotification::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  reader.RequireEnd();
  return {};
}

ClanPacket ClanLeaveRequest::Encode() const { return {kMessageId, {}}; }
ClanLeaveRequest ClanLeaveRequest::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  reader.RequireEnd();
  return {};
}

ClanPacket ClanLeaveNotification::Encode() const {
  CheckSize(12, {clan_name, character_name});
  PacketWriter writer;
  WriteText(writer, clan_name);
  writer.WriteUInt32(character_id);
  WriteText(writer, character_name);
  writer.WriteUInt32(old_status);
  writer.WriteUInt32(new_status);
  return {kMessageId, writer.TakeBytes()};
}
ClanLeaveNotification ClanLeaveNotification::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanLeaveNotification message;
  message.clan_name = ReadText(reader);
  message.character_id = reader.ReadUInt32();
  message.character_name = ReadText(reader);
  message.old_status = reader.ReadUInt32();
  message.new_status = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMemberChangeRequest::Encode() const {
  CheckSize(12, {character_name});
  PacketWriter writer;
  writer.WriteUInt32(character_id);
  WriteText(writer, character_name);
  writer.WriteUInt32(old_status);
  writer.WriteUInt32(new_status);
  return {kMessageId, writer.TakeBytes()};
}
ClanMemberChangeRequest ClanMemberChangeRequest::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMemberChangeRequest message;
  message.character_id = reader.ReadUInt32();
  message.character_name = ReadText(reader);
  message.old_status = reader.ReadUInt32();
  message.new_status = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMemberChangeResponse::Encode() const {
  CheckSize(12, {character_name});
  PacketWriter writer;
  writer.WriteUInt32(result);
  WriteText(writer, character_name);
  writer.WriteUInt32(new_status);
  writer.WriteUInt32(old_status);
  return {kMessageId, writer.TakeBytes()};
}
ClanMemberChangeResponse ClanMemberChangeResponse::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMemberChangeResponse message;
  message.result = reader.ReadUInt32();
  message.character_name = ReadText(reader);
  message.new_status = reader.ReadUInt32();
  message.old_status = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMemberChangedNotification::Encode() const {
  CheckSize(12, {clan_name, character_name});
  PacketWriter writer;
  WriteText(writer, clan_name);
  writer.WriteUInt32(character_id);
  WriteText(writer, character_name);
  writer.WriteUInt32(new_status);
  writer.WriteUInt32(old_status);
  return {kMessageId, writer.TakeBytes()};
}
ClanMemberChangedNotification ClanMemberChangedNotification::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMemberChangedNotification message;
  message.clan_name = ReadText(reader);
  message.character_id = reader.ReadUInt32();
  message.character_name = ReadText(reader);
  message.new_status = reader.ReadUInt32();
  message.old_status = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanCancelJoinRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(clan_id);
  return {kMessageId, writer.TakeBytes()};
}
ClanCancelJoinRequest ClanCancelJoinRequest::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanCancelJoinRequest message;
  message.clan_id = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanCancelJoinResponse::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(value_at4);
  return {kMessageId, writer.TakeBytes()};
}
ClanCancelJoinResponse ClanCancelJoinResponse::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanCancelJoinResponse message;
  message.result = reader.ReadUInt32();
  message.value_at4 = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanJoinCancelledNotification::Encode() const {
  CheckSize(8, {character_name});
  PacketWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(value_at4);
  WriteText(writer, character_name);
  return {kMessageId, writer.TakeBytes()};
}
ClanJoinCancelledNotification ClanJoinCancelledNotification::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanJoinCancelledNotification message;
  message.result = reader.ReadUInt32();
  message.value_at4 = reader.ReadUInt32();
  message.character_name = ReadText(reader);
  reader.RequireEnd();
  return message;
}

}  // namespace bubble_fighter::server::protocol
