#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CHARACTERS_FIGHTER_INFO_REQUEST_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CHARACTERS_FIGHTER_INFO_REQUEST_H_

#include <array>
#include <cstdint>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// E070: 0x009319E0 builds and sends this request from the client's 战士信息
// entry (dispatched by the popup handler 0x009446C0 case 2). It always writes
// exactly two 32-bit values - the local object pointer and user-stat +48 - and
// the client reads no response body: the page itself opens on S133 alone.
struct FighterInfoRequest {
  static constexpr std::uint16_t kMessageId = 0x0076;
  // Opaque client values. Their meaning is not established; they are preserved
  // so a mismatched encoder can never be mistaken for the recovered layout.
  std::array<std::uint32_t, 2> values{};

  TcpPacket Encode() const;
  static FighterInfoRequest Decode(const TcpPacket& packet);
  bool operator==(const FighterInfoRequest&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CHARACTERS_FIGHTER_INFO_REQUEST_H_
