#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_CONFIGURATION_REQUEST_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_CONFIGURATION_REQUEST_H_
#include <array>
#include <cstdint>
#include <string>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// BFCLIENT02 0x5E1490 -> 0x10D3F80. Offsets refer to the client object.
struct RoomConfigurationRequest {
  static constexpr std::uint16_t kMessageId = 513;
  std::u16string map_name, map_directory, map_key;
  std::array<std::uint32_t, 2> bytes_at72{};
  std::array<std::uint32_t, 9> values_at76{};
  std::array<std::uint32_t, 2> bytes_at112{};
  TcpPacket Encode() const;
  static RoomConfigurationRequest Decode(const TcpPacket& packet);
  bool operator==(const RoomConfigurationRequest&) const = default;
};
}  // namespace bubble_fighter::server::protocol
#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_CONFIGURATION_REQUEST_H_
