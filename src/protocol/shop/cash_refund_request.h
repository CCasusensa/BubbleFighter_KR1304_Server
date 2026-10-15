#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_CASH_REFUND_REQUEST_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_CASH_REFUND_REQUEST_H_

#include <string>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// BFCLIENT02 D3E0B0: the observed cash-card UI supplies empty text.
// Its meaning is unknown; it is never an authentication credential here.
struct CashRefundRequest {
  static constexpr std::uint16_t kMessageId = 817;
  std::u16string text_at0;
  std::uint32_t slot_id = 0;
  TcpPacket Encode() const;
  static CashRefundRequest Decode(const TcpPacket& packet);
  bool operator==(const CashRefundRequest&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_CASH_REFUND_REQUEST_H_
