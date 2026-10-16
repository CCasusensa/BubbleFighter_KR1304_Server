#include "protocol/rooms/seat_state_request.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
TcpPacket SeatStateRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(slot);
  writer.WriteUInt32(closed);
  return {kMessageId, writer.TakeBytes()};
}

SeatStateRequest SeatStateRequest::Decode(const TcpPacket& packet) {
  if (packet.message_id != kMessageId) {
    throw std::invalid_argument("Unexpected seat-state request id.");
  }
  PacketReader reader(packet.payload);
  SeatStateRequest request;
  request.slot = reader.ReadUInt32();
  request.closed = reader.ReadUInt32();
  reader.RequireEnd();
  // 0x00980300 and 0x0096A730 only produce these two states; a second
  // state value would silently invert the host's click.
  if (request.closed > 1) {
    throw std::runtime_error("Invalid seat-state request value.");
  }
  return request;
}
}  // namespace bubble_fighter::server::protocol
