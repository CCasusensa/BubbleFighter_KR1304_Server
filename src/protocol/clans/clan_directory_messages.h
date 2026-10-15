#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_DIRECTORY_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_DIRECTORY_MESSAGES_H_

#include <string>
#include <vector>

#include "protocol/clans/clan_packet.h"

namespace bubble_fighter::server::protocol {
// Query UI emits 4 name, 5/8 point, 6/7 refresh. Modes 7/8 filter recruiting.
// Wire bits do not establish search policy, authority or business limits.
struct ClanDirectoryRequest {
  static constexpr std::uint16_t kMessageId = 1019;
  std::uint32_t query_mode = 0;
  std::uint32_t page = 0;
  std::u16string search_text;
  ClanPacket Encode() const;
  static ClanDirectoryRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanDirectoryRequest&) const = default;
};
// Named fields trace through the native row constructor to UI controls.
// at* denotes native memory offsets. QWORD DATE fields retain raw bits.
struct ClanDirectoryRecord {
  std::uint32_t clan_id = 0;
  std::u16string clan_name;
  std::uint32_t value_at88 = 0;
  std::u16string master_name;
  std::uint32_t level = 0;
  std::uint32_t points = 0;
  std::uint64_t created_at_bits = 0;
  std::uint64_t value_at56_bits = 0;
  std::uint32_t recruitment_mode = 0;
  std::uint32_t member_count = 0;
  // The original parser advances past this DWORD without storing it.
  std::uint32_t value_after_member_count = 0;
  std::u16string introduction;
  std::uint64_t disband_at_bits = 0;
  std::uint64_t last_login_bits = 0;
  std::uint32_t mark_id = 0;
  bool operator==(const ClanDirectoryRecord&) const = default;
};
// The native UI displays five rows and disables next when fewer arrive.
// Codec capacity follows the frame; the five-row page belongs in business.
struct ClanDirectoryResponse {
  static constexpr std::uint16_t kMessageId = 1020;
  std::uint32_t value_at4 = 0;
  std::uint32_t page = 0;
  std::u16string text_at12;
  std::vector<ClanDirectoryRecord> clans;
  ClanPacket Encode() const;
  static ClanDirectoryResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanDirectoryResponse&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_DIRECTORY_MESSAGES_H_
