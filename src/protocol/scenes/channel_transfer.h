#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_CHANNEL_TRANSFER_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_CHANNEL_TRANSFER_H_

#include <array>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// BFCLIENT02 56FB50: a category, not an endpoint or room identifier.
struct MoveChannelRequest {
  static constexpr std::uint16_t kMessageId = 53;
  std::uint32_t channel_category = 0;
  TcpPacket Encode() const;
  static MoveChannelRequest Decode(const TcpPacket& packet);
  bool operator==(const MoveChannelRequest&) const = default;
};

// BFCLIENT02 99DD00: first application message on a moved connection.
// Neither field proves account ownership. No Passport is serialized here.
struct MoveIntoRequest {
  static constexpr std::uint16_t kMessageId = 54;
  std::uint32_t character_id = 0;
  std::uint32_t unknown_ui_value = 0;
  TcpPacket Encode() const;
  static MoveIntoRequest Decode(const TcpPacket& packet);
  bool operator==(const MoveIntoRequest&) const = default;
};

// BFCLIENT02 9A6440/945820. All fourteen bytes are read on either result.
struct MoveChannelResponse {
  static constexpr std::uint16_t kMessageId = 8;
  // Zero starts a new connection. Nonzero selects a native failure message.
  static constexpr std::uint32_t kUnavailable = 2;
  std::uint32_t result = kUnavailable;
  // inet_addr memory bytes, in textual octet order; port is little-endian.
  std::array<std::uint8_t, 4> ipv4_address{};
  std::uint16_t port = 0;
  std::uint32_t channel_category = 0;
  TcpPacket Encode() const;
  static MoveChannelResponse Decode(const TcpPacket& packet);
  bool operator==(const MoveChannelResponse&) const = default;
};

// 9A6BA0 emits C55 on the connection that received S9, after switching globals.
struct ChannelSwitchAcknowledgementRequest {
  static constexpr std::uint16_t kMessageId = 55;
  TcpPacket Encode() const;
  static ChannelSwitchAcknowledgementRequest Decode(const TcpPacket& packet);
};

// The following handlers consume no payload. The codec requires exact
// emptiness.
struct ChannelDisconnectingNotification {
  static constexpr std::uint16_t kMessageId = 9;
  TcpPacket Encode() const;
  static ChannelDisconnectingNotification Decode(const TcpPacket& packet);
};
struct ChannelDisconnectNotification {
  static constexpr std::uint16_t kMessageId = 10;
  TcpPacket Encode() const;
  static ChannelDisconnectNotification Decode(const TcpPacket& packet);
};
struct FirstChannelMoveNotification {
  static constexpr std::uint16_t kMessageId = 163;
  TcpPacket Encode() const;
  static FirstChannelMoveNotification Decode(const TcpPacket& packet);
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_CHANNEL_TRANSFER_H_
