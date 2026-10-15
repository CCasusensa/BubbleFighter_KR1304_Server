#include "protocol/shop/cash_refund_response.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
TcpPacket CashRefundResponse::Encode() const {
  PacketWriter writer;
  writer.WriteUInt32(slot_id);
  writer.WriteUInt32(result);
  writer.WriteUtf16(text_at8);
  return {kMessageId, writer.TakeBytes()};
}

CashRefundResponse CashRefundResponse::Decode(const TcpPacket& packet) {
  if (packet.message_id != kMessageId) {
    throw std::invalid_argument("Invalid cash refund response message.");
  }
  PacketReader reader(packet.payload);
  CashRefundResponse result;
  result.slot_id = reader.ReadUInt32();
  result.result = reader.ReadUInt32();
  result.text_at8 = reader.ReadUtf16();
  reader.RequireEnd();
  return result;
}
}  // namespace bubble_fighter::server::protocol
