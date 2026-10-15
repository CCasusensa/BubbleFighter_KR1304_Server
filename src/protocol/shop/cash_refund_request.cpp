#include "protocol/shop/cash_refund_request.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
TcpPacket CashRefundRequest::Encode() const {
  PacketWriter writer;
  writer.WriteUtf16(text_at0);
  writer.WriteUInt32(slot_id);
  return {kMessageId, writer.TakeBytes()};
}

CashRefundRequest CashRefundRequest::Decode(const TcpPacket& packet) {
  if (packet.message_id != kMessageId) {
    throw std::invalid_argument("Invalid cash refund request message.");
  }
  PacketReader reader(packet.payload);
  CashRefundRequest result;
  result.text_at0 = reader.ReadUtf16();
  result.slot_id = reader.ReadUInt32();
  reader.RequireEnd();
  return result;
}
}  // namespace bubble_fighter::server::protocol
