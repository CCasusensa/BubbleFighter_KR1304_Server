#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_STATE_REPORT_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_STATE_REPORT_H_

#include <string>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// Main connection report emitted by native 519160 after clan UI updates.
// These are client observations, never authoritative membership or permissions.
struct ClanStateReport {
  static constexpr std::uint16_t kMessageId = 2025;
  std::uint32_t clan_id = 0;
  std::u16string clan_name;
  std::uint32_t membership_status = 0;
  TcpPacket Encode() const;
  static ClanStateReport Decode(const TcpPacket& packet);
  bool operator==(const ClanStateReport&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_STATE_REPORT_H_
