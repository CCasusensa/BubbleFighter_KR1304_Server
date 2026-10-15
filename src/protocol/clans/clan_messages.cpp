#include "protocol/clans/clan_messages.h"

#include <stdexcept>
#include <string_view>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
constexpr auto kLimit = ClanPacketCodec::kMaxPayloadSize;
void CheckMessage(std::uint16_t actual, std::uint16_t expected,
                  std::size_t size) {
  if (actual != expected)
    throw std::invalid_argument("Unexpected clan message id.");
  if (size > kLimit)
    throw std::invalid_argument("Clan message exceeds native frame capacity.");
}
void CheckText(std::u16string_view text) {
  if (text.size() > (kLimit - 2) / 2 ||
      text.find(u'\0') != std::u16string_view::npos)
    throw std::invalid_argument(
        "Clan string cannot be represented as UTF-16Z.");
}
void WriteText(PacketWriter& writer, std::u16string_view text) {
  CheckText(text);
  for (const auto code_unit : text)
    writer.WriteUInt16(static_cast<std::uint16_t>(code_unit));
  writer.WriteUInt16(0);
}
std::u16string ReadText(PacketReader& reader) {
  std::u16string text;
  for (;;) {
    // ReadUInt16 checks both bytes before access, including a missing NUL.
    const auto code_unit = reader.ReadUInt16();
    if (!code_unit) return text;
    text.push_back(static_cast<char16_t>(code_unit));
  }
}
void CheckCredentials(const ClanCredentials& credentials, bool main) {
  CheckText(credentials.text_at4);
  const auto prefix = (main ? 20 : 18) + 2 * credentials.text_at4.size();
  if (prefix > kLimit || credentials.passport.size() > kLimit - prefix)
    throw std::invalid_argument("Clan message exceeds native frame capacity.");
}
void WriteCredentialsTail(PacketWriter& writer,
                          const ClanCredentials& credentials) {
  writer.WriteUInt32(credentials.value_at28);
  writer.WriteUInt32(credentials.value_at32);
  writer.WriteUInt32(static_cast<std::uint32_t>(credentials.passport.size()));
  for (const auto byte : credentials.passport) writer.WriteUInt8(byte);
}
ClanCredentials ReadCredentials(PacketReader& reader, bool main) {
  ClanCredentials credentials;
  credentials.value_at0 = reader.ReadUInt32();
  credentials.text_at4 = main ? reader.ReadUtf16() : ReadText(reader);
  CheckText(credentials.text_at4);
  credentials.value_at28 = reader.ReadUInt32();
  credentials.value_at32 = reader.ReadUInt32();
  const auto length = reader.ReadUInt32();
  // The native vector reader may stop at EOF. This server requires the exact
  // declared blob, with no allocation from an unchecked wire count.
  if (length != reader.RemainingBytes())
    throw std::invalid_argument("Clan Passport length does not match payload.");
  credentials.passport.reserve(length);
  for (std::uint32_t i = 0; i < length; ++i)
    credentials.passport.push_back(reader.ReadUInt8());
  return credentials;
}
template <class Packet>
void Empty(const Packet& packet, std::uint16_t expected) {
  CheckMessage(packet.message_id, expected, packet.payload.size());
  PacketReader(packet.payload).RequireEnd();
}
}  // namespace

TcpPacket ClanPassportRequest::Encode() const { return {kMessageId, {}}; }
ClanPassportRequest ClanPassportRequest::Decode(const TcpPacket& packet) {
  Empty(packet, kMessageId);
  return {};
}
TcpPacket ClanPassportResponse::Encode() const {
  CheckCredentials(credentials, true);
  PacketWriter writer;
  writer.WriteUInt32(credentials.value_at0);
  writer.WriteUtf16(credentials.text_at4);
  WriteCredentialsTail(writer, credentials);
  return {kMessageId, writer.TakeBytes()};
}
ClanPassportResponse ClanPassportResponse::Decode(const TcpPacket& packet) {
  CheckMessage(packet.message_id, kMessageId, packet.payload.size());
  PacketReader reader(packet.payload);
  return {ReadCredentials(reader, true)};
}
ClanPacket ClanHelloNotification::Encode() const { return {kMessageId, {}}; }
ClanHelloNotification ClanHelloNotification::Decode(const ClanPacket& packet) {
  Empty(packet, kMessageId);
  return {};
}
ClanPacket ClanHeartbeatRequest::Encode() const { return {kMessageId, {}}; }
ClanHeartbeatRequest ClanHeartbeatRequest::Decode(const ClanPacket& packet) {
  Empty(packet, kMessageId);
  return {};
}
ClanPacket ClanLoginRequest::Encode() const {
  CheckCredentials(credentials, false);
  PacketWriter writer;
  writer.WriteUInt32(credentials.value_at0);
  WriteText(writer, credentials.text_at4);
  WriteCredentialsTail(writer, credentials);
  return {kMessageId, writer.TakeBytes()};
}
ClanLoginRequest ClanLoginRequest::Decode(const ClanPacket& packet) {
  CheckMessage(packet.message_id, kMessageId, packet.payload.size());
  PacketReader reader(packet.payload);
  return {ReadCredentials(reader, false)};
}
ClanPacket ClanLoginResponse::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(result);
  return {kMessageId, writer.TakeBytes()};
}
ClanLoginResponse ClanLoginResponse::Decode(const ClanPacket& packet) {
  CheckMessage(packet.message_id, kMessageId, packet.payload.size());
  PacketReader reader(packet.payload);
  ClanLoginResponse message{reader.ReadUInt32()};
  reader.RequireEnd();
  return message;
}
ClanPacket ClanProfileRequest::Encode() const { return {kMessageId, {}}; }
ClanProfileRequest ClanProfileRequest::Decode(const ClanPacket& packet) {
  Empty(packet, kMessageId);
  return {};
}
ClanPacket ClanProfileResponse::Encode() const {
  CheckText(clan_name);
  if (19 + 2 * clan_name.size() > kLimit)
    throw std::invalid_argument("Clan message exceeds native frame capacity.");
  PacketWriter writer;
  writer.WriteUInt8(details_available);
  writer.WriteUInt32(clan_id);
  writer.WriteUInt32(membership_status);
  WriteText(writer, clan_name);
  writer.WriteUInt64(disband_date_bits);
  return {kMessageId, writer.TakeBytes()};
}
ClanProfileResponse ClanProfileResponse::Decode(const ClanPacket& packet) {
  CheckMessage(packet.message_id, kMessageId, packet.payload.size());
  PacketReader reader(packet.payload);
  ClanProfileResponse message;
  message.details_available = reader.ReadUInt8();
  message.clan_id = reader.ReadUInt32();
  message.membership_status = reader.ReadUInt32();
  message.clan_name = ReadText(reader);
  message.disband_date_bits = reader.ReadUInt64();
  reader.RequireEnd();
  return message;
}
ClanPacket ClanDetailsRequest::Encode() const { return {kMessageId, {}}; }
ClanDetailsRequest ClanDetailsRequest::Decode(const ClanPacket& packet) {
  Empty(packet, kMessageId);
  return {};
}
ClanPacket ClanDetailsResponse::Encode() const {
  // Check individual strings before summing, so size arithmetic is bounded.
  std::size_t size = 62;
  for (const auto* text :
       {&clan_name, &text_at76, &text_at100, &text_at176, &text_at200}) {
    CheckText(*text);
    size += 2 * text->size();
  }
  if (size > kLimit)
    throw std::invalid_argument("Clan message exceeds native frame capacity.");
  PacketWriter writer;
  WriteText(writer, clan_name);
  writer.WriteUInt32(value_at124);
  WriteText(writer, text_at76);
  writer.WriteUInt32(value_at128);
  WriteText(writer, text_at100);
  writer.WriteUInt32(value_at132);
  writer.WriteUInt32(value_at136);
  writer.WriteUInt64(date_at144_bits);
  writer.WriteUInt64(value_at152_bits);
  writer.WriteUInt32(recruiting);
  writer.WriteUInt32(value_at164);
  writer.WriteUInt32(value_at172);
  WriteText(writer, text_at176);
  WriteText(writer, text_at200);
  writer.WriteUInt32(mark_id);
  writer.WriteUInt32(value_at168);
  return {kMessageId, writer.TakeBytes()};
}
ClanDetailsResponse ClanDetailsResponse::Decode(const ClanPacket& packet) {
  CheckMessage(packet.message_id, kMessageId, packet.payload.size());
  PacketReader reader(packet.payload);
  ClanDetailsResponse message;
  message.clan_name = ReadText(reader);
  message.value_at124 = reader.ReadUInt32();
  message.text_at76 = ReadText(reader);
  message.value_at128 = reader.ReadUInt32();
  message.text_at100 = ReadText(reader);
  message.value_at132 = reader.ReadUInt32();
  message.value_at136 = reader.ReadUInt32();
  message.date_at144_bits = reader.ReadUInt64();
  message.value_at152_bits = reader.ReadUInt64();
  message.recruiting = reader.ReadUInt32();
  message.value_at164 = reader.ReadUInt32();
  message.value_at172 = reader.ReadUInt32();
  message.text_at176 = ReadText(reader);
  message.text_at200 = ReadText(reader);
  message.mark_id = reader.ReadUInt32();
  message.value_at168 = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}
ClanPacket ClanCreateRequest::Encode() const {
  CheckText(name);
  CheckText(introduction);
  if (16 + 2 * (name.size() + introduction.size()) > kLimit)
    throw std::invalid_argument("Clan message exceeds native frame capacity.");
  PacketWriter writer;
  WriteText(writer, name);
  writer.WriteUInt32(value_after_name);
  WriteText(writer, introduction);
  writer.WriteUInt32(item_id);
  writer.WriteUInt32(item_parameter);
  return {kMessageId, writer.TakeBytes()};
}
ClanCreateRequest ClanCreateRequest::Decode(const ClanPacket& packet) {
  CheckMessage(packet.message_id, kMessageId, packet.payload.size());
  PacketReader reader(packet.payload);
  ClanCreateRequest message;
  message.name = ReadText(reader);
  message.value_after_name = reader.ReadUInt32();
  message.introduction = ReadText(reader);
  message.item_id = reader.ReadUInt32();
  message.item_parameter = reader.ReadUInt32();
  reader.RequireEnd();
  return message;
}
ClanPacket ClanCreateResponse::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(value_at4);
  writer.WriteUInt32(value_at8);
  return {kMessageId, writer.TakeBytes()};
}
ClanCreateResponse ClanCreateResponse::Decode(const ClanPacket& packet) {
  CheckMessage(packet.message_id, kMessageId, packet.payload.size());
  PacketReader reader(packet.payload);
  ClanCreateResponse message{reader.ReadUInt32(), reader.ReadUInt32(),
                             reader.ReadUInt32()};
  reader.RequireEnd();
  return message;
}
}  // namespace bubble_fighter::server::protocol
