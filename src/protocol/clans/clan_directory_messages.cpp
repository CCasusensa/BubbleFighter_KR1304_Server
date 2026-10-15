#include "protocol/clans/clan_directory_messages.h"

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
void Charge(std::size_t& remaining, std::size_t bytes) {
  if (bytes > remaining)
    throw std::invalid_argument("Clan message exceeds native frame capacity.");
  remaining -= bytes;
}
void ChargeText(std::size_t& remaining, std::u16string_view text) {
  if (text.size() > (kLimit - 2) / 2 ||
      text.find(u'\0') != std::u16string_view::npos)
    throw std::invalid_argument(
        "Clan string cannot be represented as UTF-16Z.");
  Charge(remaining, 2 + 2 * text.size());
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
void WriteRow(PacketWriter& writer, const ClanDirectoryRecord& row) {
  writer.WriteUInt32(row.clan_id);
  WriteText(writer, row.clan_name);
  writer.WriteUInt32(row.value_at88);
  WriteText(writer, row.master_name);
  writer.WriteUInt32(row.level);
  writer.WriteUInt32(row.points);
  writer.WriteUInt64(row.created_at_bits);
  writer.WriteUInt64(row.value_at56_bits);
  writer.WriteUInt32(row.recruitment_mode);
  writer.WriteUInt32(row.member_count);
  writer.WriteUInt32(row.value_after_member_count);
  WriteText(writer, row.introduction);
  writer.WriteUInt64(row.disband_at_bits);
  writer.WriteUInt64(row.last_login_bits);
  writer.WriteUInt32(row.mark_id);
}
ClanDirectoryRecord ReadRow(PacketReader& reader) {
  ClanDirectoryRecord row;
  row.clan_id = reader.ReadUInt32();
  row.clan_name = ReadText(reader);
  row.value_at88 = reader.ReadUInt32();
  row.master_name = ReadText(reader);
  row.level = reader.ReadUInt32();
  row.points = reader.ReadUInt32();
  row.created_at_bits = reader.ReadUInt64();
  row.value_at56_bits = reader.ReadUInt64();
  row.recruitment_mode = reader.ReadUInt32();
  row.member_count = reader.ReadUInt32();
  row.value_after_member_count = reader.ReadUInt32();
  row.introduction = ReadText(reader);
  row.disband_at_bits = reader.ReadUInt64();
  row.last_login_bits = reader.ReadUInt64();
  row.mark_id = reader.ReadUInt32();
  return row;
}
}  // namespace

ClanPacket ClanDirectoryRequest::Encode() const {
  auto remaining = kLimit - 8;
  ChargeText(remaining, search_text);
  PacketWriter writer;
  writer.WriteUInt32(query_mode);
  writer.WriteUInt32(page);
  WriteText(writer, search_text);
  return {kMessageId, writer.TakeBytes()};
}
ClanDirectoryRequest ClanDirectoryRequest::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanDirectoryRequest message;
  message.query_mode = reader.ReadUInt32();
  message.page = reader.ReadUInt32();
  message.search_text = ReadText(reader);
  reader.RequireEnd();
  return message;
}

ClanPacket ClanDirectoryResponse::Encode() const {
  auto remaining = kLimit - 12;
  ChargeText(remaining, text_at12);
  for (const auto& row : clans) {
    Charge(remaining, 64);
    ChargeText(remaining, row.clan_name);
    ChargeText(remaining, row.master_name);
    ChargeText(remaining, row.introduction);
  }
  PacketWriter writer;
  writer.WriteUInt32(static_cast<std::uint32_t>(clans.size()));
  writer.WriteUInt32(value_at4);
  writer.WriteUInt32(page);
  WriteText(writer, text_at12);
  for (const auto& row : clans) WriteRow(writer, row);
  return {kMessageId, writer.TakeBytes()};
}
ClanDirectoryResponse ClanDirectoryResponse::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanDirectoryResponse message;
  const auto count = reader.ReadUInt32();
  message.value_at4 = reader.ReadUInt32();
  message.page = reader.ReadUInt32();
  message.text_at12 = ReadText(reader);
  // All three terminators plus eight DWORDs/four QWORDs require 70 bytes.
  if (count > reader.RemainingBytes() / 70)
    throw std::invalid_argument("Clan row count exceeds remaining payload.");
  message.clans.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i)
    message.clans.push_back(ReadRow(reader));
  reader.RequireEnd();
  return message;
}
}  // namespace bubble_fighter::server::protocol
