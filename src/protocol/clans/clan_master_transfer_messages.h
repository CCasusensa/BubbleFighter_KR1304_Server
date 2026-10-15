#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MASTER_TRANSFER_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MASTER_TRANSFER_MESSAGES_H_

#include <cstdint>
#include <string>

#include "protocol/clans/clan_packet.h"

namespace bubble_fighter::server::protocol {
// Native 1059 DWORD is echoed in 1060. Its original server meaning is unknown.
// Raw byte flags preserve wire bits; authorization must validate their values.
struct ClanMasterTransferCommitRequest {
  static constexpr std::uint16_t kMessageId = 1054;
  std::uint32_t character_id = 0;
  std::u16string character_name;
  ClanPacket Encode() const;
  static ClanMasterTransferCommitRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanMasterTransferCommitRequest&) const = default;
};
struct ClanMasterTransferredNotification {
  static constexpr std::uint16_t kMessageId = 1056;
  std::uint32_t value_at0 = 0;
  std::uint32_t former_character_id = 0;
  std::u16string former_name;
  std::uint32_t new_character_id = 0;
  std::u16string new_name;
  ClanPacket Encode() const;
  static ClanMasterTransferredNotification Decode(const ClanPacket& packet);
  bool operator==(const ClanMasterTransferredNotification&) const = default;
};
struct ClanMasterTransferRequest {
  static constexpr std::uint16_t kMessageId = 1057;
  std::uint32_t character_id = 0;
  ClanPacket Encode() const;
  static ClanMasterTransferRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanMasterTransferRequest&) const = default;
};
struct ClanMasterTransferResponse {
  static constexpr std::uint16_t kMessageId = 1058;
  std::uint8_t available = 0;
  std::uint32_t value_at1 = 0;
  ClanPacket Encode() const;
  static ClanMasterTransferResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanMasterTransferResponse&) const = default;
};
struct ClanMasterTransferOfferNotification {
  static constexpr std::uint16_t kMessageId = 1059;
  std::u16string requester_name;
  std::uint32_t correlation_value = 0;
  ClanPacket Encode() const;
  static ClanMasterTransferOfferNotification Decode(const ClanPacket& packet);
  bool operator==(const ClanMasterTransferOfferNotification&) const = default;
};
struct ClanMasterTransferAnswerRequest {
  static constexpr std::uint16_t kMessageId = 1060;
  std::uint8_t accepted = 0;
  std::uint32_t correlation_value = 0;
  ClanPacket Encode() const;
  static ClanMasterTransferAnswerRequest Decode(const ClanPacket& packet);
  bool operator==(const ClanMasterTransferAnswerRequest&) const = default;
};
struct ClanMasterTransferAnswerResponse {
  static constexpr std::uint16_t kMessageId = 1061;
  std::uint8_t success = 0;
  ClanPacket Encode() const;
  static ClanMasterTransferAnswerResponse Decode(const ClanPacket& packet);
  bool operator==(const ClanMasterTransferAnswerResponse&) const = default;
};
struct ClanMasterTransferDecisionNotification {
  static constexpr std::uint16_t kMessageId = 1062;
  std::uint8_t accepted = 0;
  std::uint32_t character_id = 0;
  std::u16string character_name;
  ClanPacket Encode() const;
  static ClanMasterTransferDecisionNotification Decode(
      const ClanPacket& packet);
  bool operator==(const ClanMasterTransferDecisionNotification&) const =
      default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MASTER_TRANSFER_MESSAGES_H_
