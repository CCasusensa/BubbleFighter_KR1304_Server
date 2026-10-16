#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_SEAT_STATE_REQUEST_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_SEAT_STATE_REQUEST_H_

#include <cstdint>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// Client writer 0x0097CFD0: {uint32 slot, uint32 state}. The room stage
// 0x00980300 sends it for a left click on an empty seat, and the slot
// context menu (0x0096A730 case 17) for the same shape; both only produce
// it while the seat is empty and open, and both send the seat's *new*
// state, computed as "the seat's closed byte is zero". 1 closes the seat,
// 0 opens it again (0x015B0360: "close every slot to start right away").
struct SeatStateRequest {
  static constexpr std::uint16_t kMessageId = 509;

  std::uint32_t slot = 0;
  std::uint32_t closed = 0;

  TcpPacket Encode() const;
  static SeatStateRequest Decode(const TcpPacket& packet);
  bool operator==(const SeatStateRequest&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_SEAT_STATE_REQUEST_H_
