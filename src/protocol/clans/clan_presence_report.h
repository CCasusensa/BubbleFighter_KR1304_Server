#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_PRESENCE_REPORT_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_PRESENCE_REPORT_H_

#include "protocol/clans/clan_packet.h"

namespace bubble_fighter::server::protocol {
// 527AB0 writes character+60 and current scene+72 through caller 517E70.
// These are untrusted observations, not membership, level or scene authority.
struct ClanPresenceReport {
  static constexpr std::uint16_t kMessageId = 1045;
  std::uint32_t reported_level = 0;
  std::uint32_t reported_scene = 0;
  ClanPacket Encode() const;
  static ClanPresenceReport Decode(const ClanPacket& packet);
  bool operator==(const ClanPresenceReport&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_PRESENCE_REPORT_H_
