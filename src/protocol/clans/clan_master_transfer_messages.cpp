#include "protocol/clans/clan_master_transfer_messages.h"

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

ClanPacket ClanMasterTransferCommitRequest::Encode() const {
  CheckSize(4, {character_name});
  PacketWriter writer;
  writer.WriteUInt32(character_id);
  WriteText(writer, character_name);
  return {kMessageId, writer.TakeBytes()};
}
ClanMasterTransferCommitRequest ClanMasterTransferCommitRequest::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMasterTransferCommitRequest message;
  message.character_id = reader.ReadUInt32();
  message.character_name = ReadText(reader);
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMasterTransferredNotification::Encode() const {
  CheckSize(12, {former_name, new_name});
  PacketWriter writer;
  writer.WriteUInt32(value_at0);
  writer.WriteUInt32(former_character_id);
  WriteText(writer, former_name);
  writer.WriteUInt32(new_character_id);
  WriteText(writer, new_name);
  return {kMessageId, writer.TakeBytes()};
}
ClanMasterTransferredNotification ClanMasterTransferredNotification::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMasterTransferredNotification message;
  message.value_at0 = reader.ReadUInt32();
  message.former_character_id = reader.ReadUInt32();
  message.former_name = ReadText(reader);
  message.new_character_id = reader.ReadUInt32();
  message.new_name = ReadText(reader);
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMasterTransferRequest::Encode() const {
  CheckSize(4, {});
  PacketWriter writer;
  writer.WriteUInt32(character_id);
  return {kMessageId, writer.TakeBytes()};
}
ClanMasterTransferRequest ClanMasterTransferRequest::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMasterTransferRequest message;
  message.character_id = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMasterTransferResponse::Encode() const {
  CheckSize(5, {});
  PacketWriter writer;
  writer.WriteUInt8(available);
  writer.WriteUInt32(value_at1);
  return {kMessageId, writer.TakeBytes()};
}
ClanMasterTransferResponse ClanMasterTransferResponse::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMasterTransferResponse message;
  message.available = reader.ReadUInt8();
  message.value_at1 = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMasterTransferOfferNotification::Encode() const {
  CheckSize(4, {requester_name});
  PacketWriter writer;
  WriteText(writer, requester_name);
  writer.WriteUInt32(correlation_value);
  return {kMessageId, writer.TakeBytes()};
}
ClanMasterTransferOfferNotification ClanMasterTransferOfferNotification::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMasterTransferOfferNotification message;
  message.requester_name = ReadText(reader);
  message.correlation_value = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMasterTransferAnswerRequest::Encode() const {
  CheckSize(5, {});
  PacketWriter writer;
  writer.WriteUInt8(accepted);
  writer.WriteUInt32(correlation_value);
  return {kMessageId, writer.TakeBytes()};
}
ClanMasterTransferAnswerRequest ClanMasterTransferAnswerRequest::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMasterTransferAnswerRequest message;
  message.accepted = reader.ReadUInt8();
  message.correlation_value = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMasterTransferAnswerResponse::Encode() const {
  CheckSize(1, {});
  PacketWriter writer;
  writer.WriteUInt8(success);
  return {kMessageId, writer.TakeBytes()};
}
ClanMasterTransferAnswerResponse ClanMasterTransferAnswerResponse::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMasterTransferAnswerResponse message;
  message.success = reader.ReadUInt8();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMasterTransferDecisionNotification::Encode() const {
  CheckSize(5, {character_name});
  PacketWriter writer;
  writer.WriteUInt8(accepted);
  writer.WriteUInt32(character_id);
  WriteText(writer, character_name);
  return {kMessageId, writer.TakeBytes()};
}
ClanMasterTransferDecisionNotification
ClanMasterTransferDecisionNotification::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMasterTransferDecisionNotification message;
  message.accepted = reader.ReadUInt8();
  message.character_id = reader.ReadUInt32();
  message.character_name = ReadText(reader);
  reader.RequireEnd();
  return message;
}

}  // namespace bubble_fighter::server::protocol
