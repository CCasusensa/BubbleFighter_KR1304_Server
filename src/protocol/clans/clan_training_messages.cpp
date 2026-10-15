#include "protocol/clans/clan_training_messages.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
template <class Packet>
void Check(const Packet& packet, std::uint16_t id, std::size_t size) {
  if (packet.message_id != id || packet.payload.size() != size)
    throw std::invalid_argument("Invalid training clan packet.");
}
bool Positive(std::uint32_t value) { return value && value <= 0x7fffffffU; }
}  // namespace
ClanPacket TrainingClanJoinRequest::Encode() const { return {kMessageId, {}}; }
TrainingClanJoinRequest TrainingClanJoinRequest::Decode(
    const ClanPacket& packet) {
  Check(packet, kMessageId, 0);
  return {};
}
ClanPacket TrainingClanJoinResponse::Encode() const {
  const bool success = Positive(clan_id_or_failure);
  if ((!success && (membership_status || !clan_name.empty())) ||
      clan_name.find(u'\0') != std::u16string::npos ||
      clan_name.size() > (ClanPacketCodec::kMaxPayloadSize - 10) / 2)
    throw std::invalid_argument("Invalid training clan response.");
  PacketWriter writer;
  writer.WriteUInt32(clan_id_or_failure);
  if (success) {
    writer.WriteUInt32(membership_status);
    for (const auto unit : clan_name) writer.WriteUInt16(unit);
    writer.WriteUInt16(0);
  }
  return {kMessageId, writer.TakeBytes()};
}
TrainingClanJoinResponse TrainingClanJoinResponse::Decode(
    const ClanPacket& packet) {
  if (packet.message_id != kMessageId ||
      packet.payload.size() > ClanPacketCodec::kMaxPayloadSize)
    throw std::invalid_argument("Invalid training clan response.");
  PacketReader reader(packet.payload);
  TrainingClanJoinResponse result;
  result.clan_id_or_failure = reader.ReadUInt32();
  if (Positive(result.clan_id_or_failure)) {
    result.membership_status = reader.ReadUInt32();
    for (;;) {
      const auto unit = reader.ReadUInt16();
      if (!unit) break;
      result.clan_name.push_back(static_cast<char16_t>(unit));
    }
  }
  reader.RequireEnd();
  return result;
}
ClanPacket TrainingClanExitRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(new_status);
  return {kMessageId, writer.TakeBytes()};
}
TrainingClanExitRequest TrainingClanExitRequest::Decode(
    const ClanPacket& packet) {
  Check(packet, kMessageId, 4);
  return {PacketReader(packet.payload).ReadUInt32()};
}
TcpPacket ClanAppearanceReport::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(clan_id);
  writer.WriteUInt32(value_at4);
  writer.WriteUInt32(mark_id);
  return {kMessageId, writer.TakeBytes()};
}
ClanAppearanceReport ClanAppearanceReport::Decode(const TcpPacket& packet) {
  Check(packet, kMessageId, 12);
  PacketReader reader(packet.payload);
  return {reader.ReadUInt32(), reader.ReadUInt32(), reader.ReadUInt32()};
}
TcpPacket TrainingClanGraduationNotification::Encode() const {
  return {kMessageId, {}};
}
TrainingClanGraduationNotification TrainingClanGraduationNotification::Decode(
    const TcpPacket& packet) {
  Check(packet, kMessageId, 0);
  return {};
}
}  // namespace bubble_fighter::server::protocol
