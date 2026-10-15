#include "protocol/clans/clan_roster_messages.h"

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
void ChargeRow(std::size_t& remaining, std::size_t scalar_bytes,
               std::u16string_view text) {
  if (text.size() > (kLimit - 2) / 2 ||
      text.find(u'\0') != std::u16string_view::npos)
    throw std::invalid_argument(
        "Clan string cannot be represented as UTF-16Z.");
  const auto size = scalar_bytes + 2 + 2 * text.size();
  if (size > remaining)
    throw std::invalid_argument("Clan message exceeds native frame capacity.");
  remaining -= size;
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
std::uint32_t ReadCount(PacketReader& reader, std::size_t minimum_row_size) {
  const auto count = reader.ReadUInt32();
  // Bound allocation by actual remaining bytes, never only the wire count.
  if (count > reader.RemainingBytes() / minimum_row_size)
    throw std::invalid_argument("Clan row count exceeds remaining payload.");
  return count;
}
void WriteRow(PacketWriter& writer, const ClanMemberRecord& row) {
  writer.WriteUInt32(row.character_id);
  WriteText(writer, row.character_name);
  writer.WriteUInt32(row.value_at36);
  writer.WriteUInt32(row.value_at28);
  writer.WriteUInt32(row.membership_status);
}
ClanMemberRecord ReadClanMemberRecord(PacketReader& reader) {
  ClanMemberRecord row;
  row.character_id = reader.ReadUInt32();
  row.character_name = ReadText(reader);
  row.value_at36 = reader.ReadUInt32();
  row.value_at28 = reader.ReadUInt32();
  row.membership_status = reader.ReadUInt32();
  return row;
}
void WriteRow(PacketWriter& writer, const ClanMemberDetailsRecord& row) {
  writer.WriteUInt32(row.character_id);
  WriteText(writer, row.character_name);
  writer.WriteUInt32(row.value_at28);
  writer.WriteUInt32(row.membership_status);
  writer.WriteUInt32(row.value_at40);
  writer.WriteUInt32(row.value_at44);
  writer.WriteUInt64(row.value_at48_bits);
  writer.WriteUInt64(row.value_at56_bits);
  writer.WriteUInt64(row.value_at64_bits);
  writer.WriteUInt32(row.value_at72);
  writer.WriteUInt32(row.value_at76);
  writer.WriteUInt32(row.value_at80);
  writer.WriteUInt32(row.value_at84);
}
ClanMemberDetailsRecord ReadClanMemberDetailsRecord(PacketReader& reader) {
  ClanMemberDetailsRecord row;
  row.character_id = reader.ReadUInt32();
  row.character_name = ReadText(reader);
  row.value_at28 = reader.ReadUInt32();
  row.membership_status = reader.ReadUInt32();
  row.value_at40 = reader.ReadUInt32();
  row.value_at44 = reader.ReadUInt32();
  row.value_at48_bits = reader.ReadUInt64();
  row.value_at56_bits = reader.ReadUInt64();
  row.value_at64_bits = reader.ReadUInt64();
  row.value_at72 = reader.ReadUInt32();
  row.value_at76 = reader.ReadUInt32();
  row.value_at80 = reader.ReadUInt32();
  row.value_at84 = reader.ReadUInt32();
  return row;
}
void WriteRow(PacketWriter& writer, const ClanApplicantRecord& row) {
  writer.WriteUInt32(row.character_id);
  WriteText(writer, row.character_name);
  writer.WriteUInt32(row.value_at28);
  writer.WriteUInt32(row.membership_status);
  writer.WriteUInt64(row.value_at56_bits);
  writer.WriteUInt32(row.value_at40);
  writer.WriteUInt32(row.value_at44);
  writer.WriteUInt32(row.value_at48);
  writer.WriteUInt32(row.value_at52);
}
ClanApplicantRecord ReadClanApplicantRecord(PacketReader& reader) {
  ClanApplicantRecord row;
  row.character_id = reader.ReadUInt32();
  row.character_name = ReadText(reader);
  row.value_at28 = reader.ReadUInt32();
  row.membership_status = reader.ReadUInt32();
  row.value_at56_bits = reader.ReadUInt64();
  row.value_at40 = reader.ReadUInt32();
  row.value_at44 = reader.ReadUInt32();
  row.value_at48 = reader.ReadUInt32();
  row.value_at52 = reader.ReadUInt32();
  return row;
}
}  // namespace

ClanPacket ClanRosterRequest::Encode() const { return {kMessageId, {}}; }
ClanRosterRequest ClanRosterRequest::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  reader.RequireEnd();
  return {};
}

ClanPacket ClanRosterResponse::Encode() const {
  auto remaining = kLimit - 4;
  for (const auto& row : members) ChargeRow(remaining, 16, row.character_name);
  PacketWriter writer;
  writer.WriteUInt32(static_cast<std::uint32_t>(members.size()));
  for (const auto& row : members) WriteRow(writer, row);
  return {kMessageId, writer.TakeBytes()};
}
ClanRosterResponse ClanRosterResponse::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanRosterResponse message;
  const auto count = ReadCount(reader, 18);
  message.members.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i)
    message.members.push_back(ReadClanMemberRecord(reader));
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMemberDetailsRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(value_at0);
  return {kMessageId, writer.TakeBytes()};
}
ClanMemberDetailsRequest ClanMemberDetailsRequest::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMemberDetailsRequest message;
  message.value_at0 = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanMemberDetailsResponse::Encode() const {
  auto remaining = kLimit - 8;
  for (const auto& row : members) ChargeRow(remaining, 60, row.character_name);
  PacketWriter writer;
  writer.WriteUInt32(value_at0);
  writer.WriteUInt32(static_cast<std::uint32_t>(members.size()));
  for (const auto& row : members) WriteRow(writer, row);
  return {kMessageId, writer.TakeBytes()};
}
ClanMemberDetailsResponse ClanMemberDetailsResponse::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanMemberDetailsResponse message;
  message.value_at0 = reader.ReadUInt32();
  const auto count = ReadCount(reader, 62);
  message.members.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i)
    message.members.push_back(ReadClanMemberDetailsRecord(reader));
  reader.RequireEnd();
  return message;
}

ClanPacket ClanApplicantsRequest::Encode() const { return {kMessageId, {}}; }
ClanApplicantsRequest ClanApplicantsRequest::Decode(const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  reader.RequireEnd();
  return {};
}

ClanPacket ClanApplicantsResponse::Encode() const {
  auto remaining = kLimit - 4;
  for (const auto& row : applicants)
    ChargeRow(remaining, 36, row.character_name);
  PacketWriter writer;
  writer.WriteUInt32(static_cast<std::uint32_t>(applicants.size()));
  for (const auto& row : applicants) WriteRow(writer, row);
  return {kMessageId, writer.TakeBytes()};
}
ClanApplicantsResponse ClanApplicantsResponse::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanApplicantsResponse message;
  const auto count = ReadCount(reader, 38);
  message.applicants.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i)
    message.applicants.push_back(ReadClanApplicantRecord(reader));
  reader.RequireEnd();
  return message;
}

ClanPacket ClanRecruitmentRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(clan_id);
  writer.WriteUInt32(recruitment_mode);
  return {kMessageId, writer.TakeBytes()};
}
ClanRecruitmentRequest ClanRecruitmentRequest::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanRecruitmentRequest message;
  message.clan_id = reader.ReadUInt32();
  message.recruitment_mode = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

ClanPacket ClanRecruitmentResponse::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(value_at0);
  writer.WriteUInt32(result);
  writer.WriteUInt32(recruitment_mode);
  return {kMessageId, writer.TakeBytes()};
}
ClanRecruitmentResponse ClanRecruitmentResponse::Decode(
    const ClanPacket& packet) {
  CheckMessage(packet, kMessageId);
  PacketReader reader(packet.payload);
  ClanRecruitmentResponse message;
  message.value_at0 = reader.ReadUInt32();
  message.result = reader.ReadUInt32();
  message.recruitment_mode = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}

}  // namespace bubble_fighter::server::protocol
