#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_GIFTS_GIFT_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_GIFTS_GIFT_MESSAGES_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::protocol {
// Resource policies, not recovered original server limits. UI list requests
// use4.
inline constexpr std::size_t kMaxGiftElements = 1024;
inline constexpr std::size_t kMaxGiftPayloadBytes = 1024 * 1024;
// Offsets name x86 native members, not wire positions. Unknown values stay raw.
// Record strides are88 received and112 sent. Their strings are counted UTF16.
struct ReceivedGiftRecord {
  std::uint32_t gift_id = 0;
  std::uint64_t wide_at8 = 0;
  std::uint32_t value_at16 = 0;
  std::u16string text_at20;
  std::uint32_t stock_id = 0;
  std::uint16_t short_at48 = 0;
  std::u16string text_at52;
  std::uint64_t wide_at80 = 0;
  bool operator==(const ReceivedGiftRecord&) const = default;
};

struct SentGiftRecord {
  std::uint32_t gift_id = 0;
  std::uint64_t wide_at8 = 0;
  std::uint64_t wide_at16 = 0;
  std::uint32_t value_at24 = 0;
  std::u16string text_at28;
  std::uint32_t stock_id = 0;
  std::uint16_t short_at56 = 0;
  std::uint16_t short_at58 = 0;
  std::u16string text_at60;
  std::u16string text_at84;
  bool operator==(const SentGiftRecord&) const = default;
};

struct GiftDetailRecord {
  std::uint32_t value_at0 = 0;
  std::uint32_t value_at4 = 0;
  std::vector<std::uint32_t> values_at8;
  std::uint32_t value_at20 = 0;
  std::u16string text_at24;
  bool operator==(const GiftDetailRecord&) const = default;
};

struct GiftDetails {
  std::uint32_t value_at0 = 0;
  std::vector<std::uint32_t> values_at4;
  std::vector<std::uint32_t> values_at16;
  std::vector<std::uint32_t> values_at28;
  std::vector<std::uint8_t> bytes_at40;
  std::vector<GiftDetailRecord> records_at52;
  bool operator==(const GiftDetails&) const = default;
};

struct GiftReceivedListRequest {
  static constexpr std::uint16_t kMessageId = 815;
  std::uint32_t offset = 0;
  std::uint32_t count = 0;
  TcpPacket Encode() const;
  static GiftReceivedListRequest Decode(const TcpPacket& packet);
  bool operator==(const GiftReceivedListRequest&) const = default;
};

struct GiftSentListRequest {
  static constexpr std::uint16_t kMessageId = 816;
  std::uint32_t offset = 0;
  std::uint32_t count = 0;
  TcpPacket Encode() const;
  static GiftSentListRequest Decode(const TcpPacket& packet);
  bool operator==(const GiftSentListRequest&) const = default;
};

struct GiftSendRequest {
  static constexpr std::uint16_t kMessageId = 818;
  std::u16string recipient_name;
  std::u16string text_at24;
  std::uint32_t stock_id = 0;
  std::u16string message;
  TcpPacket Encode() const;
  static GiftSendRequest Decode(const TcpPacket& packet);
  bool operator==(const GiftSendRequest&) const = default;
};

struct GiftReceiveRequest {
  static constexpr std::uint16_t kMessageId = 819;
  std::uint32_t gift_id = 0;
  TcpPacket Encode() const;
  static GiftReceiveRequest Decode(const TcpPacket& packet);
  bool operator==(const GiftReceiveRequest&) const = default;
};

struct GiftDiscardRequest {
  static constexpr std::uint16_t kMessageId = 820;
  std::uint32_t gift_id = 0;
  TcpPacket Encode() const;
  static GiftDiscardRequest Decode(const TcpPacket& packet);
  bool operator==(const GiftDiscardRequest&) const = default;
};

struct GiftSendThanksRequest {
  static constexpr std::uint16_t kMessageId = 821;
  std::uint32_t gift_id = 0;
  std::u16string message;
  std::uint32_t value_at28 = 0;
  TcpPacket Encode() const;
  static GiftSendThanksRequest Decode(const TcpPacket& packet);
  bool operator==(const GiftSendThanksRequest&) const = default;
};

struct GiftViewThanksRequest {
  static constexpr std::uint16_t kMessageId = 822;
  std::uint32_t gift_id = 0;
  TcpPacket Encode() const;
  static GiftViewThanksRequest Decode(const TcpPacket& packet);
  bool operator==(const GiftViewThanksRequest&) const = default;
};

struct GiftDeleteSentRequest {
  static constexpr std::uint16_t kMessageId = 824;
  std::uint32_t gift_id = 0;
  TcpPacket Encode() const;
  static GiftDeleteSentRequest Decode(const TcpPacket& packet);
  bool operator==(const GiftDeleteSentRequest&) const = default;
};

struct GiftDeleteReceivedRequest {
  static constexpr std::uint16_t kMessageId = 825;
  std::uint32_t gift_id = 0;
  TcpPacket Encode() const;
  static GiftDeleteReceivedRequest Decode(const TcpPacket& packet);
  bool operator==(const GiftDeleteReceivedRequest&) const = default;
};

struct GiftNewReceiveNotification {
  static constexpr std::uint16_t kMessageId = 819;
  std::uint32_t value_at0 = 0;
  std::uint32_t new_count = 0;
  TcpPacket Encode() const;
  static GiftNewReceiveNotification Decode(const TcpPacket& packet);
  bool operator==(const GiftNewReceiveNotification&) const = default;
};

struct GiftReceivedListResponse {
  static constexpr std::uint16_t kMessageId = 822;
  std::uint32_t result = 0;
  std::uint32_t total_count = 0;
  std::uint32_t value_at8 = 0;
  std::vector<ReceivedGiftRecord> records;
  TcpPacket Encode() const;
  static GiftReceivedListResponse Decode(const TcpPacket& packet);
  bool operator==(const GiftReceivedListResponse&) const = default;
};

struct GiftSentListResponse {
  static constexpr std::uint16_t kMessageId = 823;
  std::uint32_t result = 0;
  std::uint32_t total_count = 0;
  std::vector<SentGiftRecord> records;
  TcpPacket Encode() const;
  static GiftSentListResponse Decode(const TcpPacket& packet);
  bool operator==(const GiftSentListResponse&) const = default;
};

struct GiftSendResponse {
  static constexpr std::uint16_t kMessageId = 825;
  std::u16string text_at0;
  std::uint32_t value_at24 = 0;
  std::uint32_t result = 0;
  std::u16string text_at32;
  std::u16string text_at56;
  TcpPacket Encode() const;
  static GiftSendResponse Decode(const TcpPacket& packet);
  bool operator==(const GiftSendResponse&) const = default;
};

struct GiftReceiveResponse {
  static constexpr std::uint16_t kMessageId = 826;
  std::uint32_t result = 0;
  std::uint32_t gift_id = 0;
  std::uint32_t value_at8 = 0;
  std::uint32_t new_count = 0;
  TcpPacket Encode() const;
  static GiftReceiveResponse Decode(const TcpPacket& packet);
  bool operator==(const GiftReceiveResponse&) const = default;
};

struct GiftDiscardResponse {
  static constexpr std::uint16_t kMessageId = 827;
  std::uint32_t result = 0;
  std::uint32_t gift_id = 0;
  std::uint32_t new_count = 0;
  TcpPacket Encode() const;
  static GiftDiscardResponse Decode(const TcpPacket& packet);
  bool operator==(const GiftDiscardResponse&) const = default;
};

struct GiftSendThanksResponse {
  static constexpr std::uint16_t kMessageId = 828;
  std::uint32_t result = 0;
  TcpPacket Encode() const;
  static GiftSendThanksResponse Decode(const TcpPacket& packet);
  bool operator==(const GiftSendThanksResponse&) const = default;
};

struct GiftViewThanksResponse {
  static constexpr std::uint16_t kMessageId = 829;
  std::uint32_t value_at0 = 0;
  std::uint32_t gift_id = 0;
  std::u16string text_at8;
  std::u16string text_at32;
  std::uint32_t value_at56 = 0;
  std::uint32_t value_at60 = 0;
  GiftDetails extra;
  TcpPacket Encode() const;
  static GiftViewThanksResponse Decode(const TcpPacket& packet);
  bool operator==(const GiftViewThanksResponse&) const = default;
};

struct GiftConfirmResponse {
  static constexpr std::uint16_t kMessageId = 830;
  std::uint32_t result = 0;
  TcpPacket Encode() const;
  static GiftConfirmResponse Decode(const TcpPacket& packet);
  bool operator==(const GiftConfirmResponse&) const = default;
};

struct GiftDeleteSentResponse {
  static constexpr std::uint16_t kMessageId = 831;
  std::uint32_t result = 0;
  std::uint32_t gift_id = 0;
  TcpPacket Encode() const;
  static GiftDeleteSentResponse Decode(const TcpPacket& packet);
  bool operator==(const GiftDeleteSentResponse&) const = default;
};

struct GiftDeleteReceivedResponse {
  static constexpr std::uint16_t kMessageId = 832;
  std::uint32_t result = 0;
  std::uint32_t gift_id = 0;
  std::uint32_t new_count = 0;
  TcpPacket Encode() const;
  static GiftDeleteReceivedResponse Decode(const TcpPacket& packet);
  bool operator==(const GiftDeleteReceivedResponse&) const = default;
};

}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_GIFTS_GIFT_MESSAGES_H_
