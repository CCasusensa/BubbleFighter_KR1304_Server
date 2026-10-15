#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_CASH_REFUND_RESPONSE_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_CASH_REFUND_RESPONSE_H_

#include <string>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// S824, gspRepRefund. Result1 deletes the cash slot; result4 reports expiry.
// Error2/3/5 have distinct generic failure messages, not proven causes.
struct CashRefundResponse {
  static constexpr std::uint16_t kMessageId = 824;
  static constexpr std::uint32_t kSuccess = 1;
  static constexpr std::uint32_t kFailed = 2;
  static constexpr std::uint32_t kExpired = 4;
  std::uint32_t slot_id = 0;
  std::uint32_t result = kFailed;
  std::u16string text_at8;
  TcpPacket Encode() const;
  static CashRefundResponse Decode(const TcpPacket& packet);
  bool operator==(const CashRefundResponse&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_CASH_REFUND_RESPONSE_H_
