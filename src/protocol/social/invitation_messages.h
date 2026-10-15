#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_INVITATION_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_INVITATION_MESSAGES_H_

#include <string>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// Native BFCLIENT02 invitation messages. These are shared by the original
// party/room UI, distinct from project CompanionSocialRequest messages.
// Field offsets refer to native objects, not positions in the wire payload.
struct InvitationRequest {
  static constexpr std::uint16_t kMessageId = 110;
  std::u16string target_name;
  // 90A330 supplies 0/1 from a channel/party eligibility check. Its complete
  // server meaning is not established; it never grants admission authority.
  std::uint32_t value_at24 = 1;
  TcpPacket Encode() const;
  static InvitationRequest Decode(const TcpPacket& packet);
};

struct DeclineInvitationRequest {
  static constexpr std::uint16_t kMessageId = 111;
  // S121 object +24 is echoed by 9D7750 on refusal.
  std::uint32_t invitation_key = 0;
  TcpPacket Encode() const;
  static DeclineInvitationRequest Decode(const TcpPacket& packet);
};

struct MoveToInviterRequest {
  static constexpr std::uint16_t kMessageId = 115;
  std::u16string inviter_name;
  // Opaque native NetAddress storage. Never route to client-supplied addresses.
  std::uint32_t address = 0;
  std::uint16_t port = 0;
  // The S121 accept path supplies 1; other callers require separate analysis.
  std::uint8_t byte_at32 = 1;
  TcpPacket Encode() const;
  static MoveToInviterRequest Decode(const TcpPacket& packet);
};

struct InvitationFailedNotification {
  static constexpr std::uint16_t kMessageId = 119;
  std::u16string target_name;
  std::uint32_t reason = 0;
  TcpPacket Encode() const;
  static InvitationFailedNotification Decode(const TcpPacket& packet);
};

struct InvitationDeliveredNotification {
  static constexpr std::uint16_t kMessageId = 120;
  std::u16string target_name;
  TcpPacket Encode() const;
  static InvitationDeliveredNotification Decode(const TcpPacket& packet);
};

struct AskedInvitationNotification {
  static constexpr std::uint16_t kMessageId = 121;
  std::u16string inviter_name;
  std::uint32_t invitation_key = 0;
  std::uint32_t address = 0;
  std::uint16_t port = 0;
  std::uint32_t value_at36 = 0;
  std::uint32_t value_at40 = 0;
  TcpPacket Encode() const;
  static AskedInvitationNotification Decode(const TcpPacket& packet);
};

struct MoveToInviterFailedNotification {
  static constexpr std::uint16_t kMessageId = 122;
  std::u16string inviter_name;
  std::uint32_t reason = 0;
  TcpPacket Encode() const;
  static MoveToInviterFailedNotification Decode(const TcpPacket& packet);
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_INVITATION_MESSAGES_H_
