#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_TRAINING_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_TRAINING_MESSAGES_H_

#include <string>

#include "protocol/clans/clan_packet.h"
#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
struct TrainingClanJoinRequest {
  static constexpr std::uint16_t kMessageId = 1106;
  ClanPacket Encode() const;
  static TrainingClanJoinRequest Decode(const ClanPacket& packet);
};
struct TrainingClanJoinResponse {
  static constexpr std::uint16_t kMessageId = 1107;
  // Native signed first DWORD: <= 0 is the entire failure payload;
  // > 0 is the clan ID, followed by status and UTF-16Z name.
  std::uint32_t clan_id_or_failure = 0;
  std::uint32_t membership_status = 0;
  std::u16string clan_name;
  ClanPacket Encode() const;
  static TrainingClanJoinResponse Decode(const ClanPacket& packet);
};
struct TrainingClanExitRequest {
  static constexpr std::uint16_t kMessageId = 1108;
  std::uint32_t new_status = 0;
  ClanPacket Encode() const;
  static TrainingClanExitRequest Decode(const ClanPacket& packet);
};
// Untrusted native report. It never grants membership or appearance authority.
struct ClanAppearanceReport {
  static constexpr std::uint16_t kMessageId = 948;
  std::uint32_t clan_id = 0;
  std::uint32_t value_at4 = 0;
  std::uint32_t mark_id = 0;
  TcpPacket Encode() const;
  static ClanAppearanceReport Decode(const TcpPacket& packet);
};
struct TrainingClanGraduationNotification {
  static constexpr std::uint16_t kMessageId = 1515;
  TcpPacket Encode() const;
  static TrainingClanGraduationNotification Decode(const TcpPacket& packet);
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_TRAINING_MESSAGES_H_
