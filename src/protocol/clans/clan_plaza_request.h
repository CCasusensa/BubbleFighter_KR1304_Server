#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_PLAZA_REQUEST_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_PLAZA_REQUEST_H_

#include <string>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// Main C2028, writer 537570: signed cached DWORD becomes a 64-bit value,
// followed by a counted UTF-16 name. Preserve wire bits at the codec boundary.
// Neither field authorizes clan membership, identity or scene admission.
struct ClanPlazaRequest {
  static constexpr std::uint16_t kMessageId = 2028;
  std::uint64_t reported_clan_id = 0;
  std::u16string reported_clan_name;
  TcpPacket Encode() const;
  static ClanPlazaRequest Decode(const TcpPacket& packet);
  bool operator==(const ClanPlazaRequest&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_PLAZA_REQUEST_H_
