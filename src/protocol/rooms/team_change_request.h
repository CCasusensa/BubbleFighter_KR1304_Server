#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_TEAM_CHANGE_REQUEST_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_TEAM_CHANGE_REQUEST_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// RoomCreation R25: C515. Three writers exist and all three open with the
// shared 0x3A/0xBA optional marker:
//   0x00ADEF30 - the room stage's blue/red button sends both markers absent
//                (0xBA 0xBA, two bytes) after checking that the local player
//                may move (0x015B0360/0x0159753C messages);
//   0x004B3AC0 - the slot context menu's "team change" sends one present
//                value (the slot that was clicked) and then a structure this
//                server does not interpret (0x004B3AC0 -> 0x0010C3E90);
//   0x004A1EB0 - the local player's own slot plus a present group of two
//                values.
// Only the first optional carries a value the room can use; every writer's
// remaining bytes are kept verbatim so a decoded request always re-encodes
// byte for byte.
struct TeamChangeRequest {
  static constexpr std::uint16_t kMessageId = 515;
  static constexpr std::uint8_t kPresentMarker = 0x3A;
  static constexpr std::uint8_t kAbsentMarker = 0xBA;

  // Absent when the client leaves the choice to the server.
  std::optional<std::uint32_t> value_at0;
  // Writer-specific remainder: 0x00ADEF30 ends with its own absent marker,
  // 0x004B3AC0 appends a structure through 0x0010C3E90 and 0x004A1EB0 a
  // present group of two values. Retained, never interpreted.
  std::vector<std::uint8_t> rest;

  TcpPacket Encode() const;
  static TeamChangeRequest Decode(const TcpPacket& packet);
  bool operator==(const TeamChangeRequest&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_TEAM_CHANGE_REQUEST_H_
