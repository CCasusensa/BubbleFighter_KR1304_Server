#include "protocol/clans/clan_plaza_request.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
TcpPacket ClanPlazaRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt64(reported_clan_id);
  writer.WriteUtf16(reported_clan_name);
  return {kMessageId, writer.TakeBytes()};
}
ClanPlazaRequest ClanPlazaRequest::Decode(const TcpPacket& packet) {
  if (packet.message_id != kMessageId)
    throw std::invalid_argument("Unexpected clan plaza request id.");
  PacketReader reader(packet.payload);
  ClanPlazaRequest result;
  result.reported_clan_id = reader.ReadUInt64();
  result.reported_clan_name = reader.ReadUtf16();
  reader.RequireEnd();
  return result;
}
}  // namespace bubble_fighter::server::protocol
