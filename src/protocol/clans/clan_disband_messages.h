#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_DISBAND_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_DISBAND_MESSAGES_H_

#include "protocol/clans/clan_packet.h"

namespace bubble_fighter::server::protocol {
struct ClanDisbandRequest {
  static constexpr std::uint16_t kMessageId = 1029;
  std::uint32_t action = 0;
  ClanPacket Encode() const;
  static ClanDisbandRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanDisbandRequest&) const = default;
};
struct ClanDisbandResponse {
  static constexpr std::uint16_t kMessageId = 1030;
  std::uint32_t result = 0;
  std::uint32_t action = 0;
  ClanPacket Encode() const;
  static ClanDisbandResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanDisbandResponse&) const = default;
};
struct ClanDisbandNotification {
  static constexpr std::uint16_t kMessageId = 1031;
  std::uint32_t result = 0;
  std::uint32_t action = 0;
  ClanPacket Encode() const;
  static ClanDisbandNotification Decode(const ClanPacket& packet);
  bool operator==(const ClanDisbandNotification&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_DISBAND_MESSAGES_H_
