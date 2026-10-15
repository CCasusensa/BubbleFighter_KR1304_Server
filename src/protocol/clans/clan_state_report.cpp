#include "protocol/clans/clan_state_report.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
TcpPacket ClanStateReport::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(clan_id);
  writer.WriteUtf16(clan_name);
  writer.WriteUInt32(membership_status);
  return {kMessageId, writer.TakeBytes()};
}
ClanStateReport ClanStateReport::Decode(const TcpPacket& packet) {
  if (packet.message_id != kMessageId)
    throw std::invalid_argument("Unexpected clan state report id.");
  PacketReader reader(packet.payload);
  ClanStateReport result;
  result.clan_id = reader.ReadUInt32();
  result.clan_name = reader.ReadUtf16();
  result.membership_status = reader.ReadUInt32();
  reader.RequireEnd();
  return result;
}
}  // namespace bubble_fighter::server::protocol
