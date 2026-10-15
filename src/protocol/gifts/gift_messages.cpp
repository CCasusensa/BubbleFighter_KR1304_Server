#include "protocol/gifts/gift_messages.h"

#include <stdexcept>
#include <string_view>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
// Check the cumulative budget before growing the underlying buffer.
class GiftWriter {
 public:
  void WriteUInt8(std::uint8_t value) {
    Add(1);
    writer_.WriteUInt8(value);
  }
  void WriteUInt16(std::uint16_t value) {
    Add(2);
    writer_.WriteUInt16(value);
  }
  void WriteUInt32(std::uint32_t value) {
    Add(4);
    writer_.WriteUInt32(value);
  }
  void WriteUInt64(std::uint64_t value) {
    Add(8);
    writer_.WriteUInt64(value);
  }
  void WriteUtf16(std::u16string_view value) {
    if (value.size() > PacketWriter::kMaxStringCodeUnits)
      throw std::invalid_argument("Gift text exceeds native reader limit.");
    Add(4 + value.size() * 2);
    writer_.WriteUtf16(value);
  }
  void Count(std::size_t count) {
    if (count > kMaxGiftElements)
      throw std::invalid_argument("Gift collection exceeds resource limit.");
    WriteUInt32(static_cast<std::uint32_t>(count));
  }
  std::vector<std::uint8_t> TakeBytes() { return writer_.TakeBytes(); }

 private:
  void Add(std::size_t bytes) {
    if (bytes > kMaxGiftPayloadBytes - size_)
      throw std::invalid_argument("Gift payload exceeds resource limit.");
    size_ += bytes;
  }
  PacketWriter writer_;
  std::size_t size_ = 0;
};

std::uint32_t ReadCount(PacketReader& reader, std::size_t minimum_bytes) {
  const auto count = reader.ReadUInt32();
  if (count > kMaxGiftElements ||
      count > reader.RemainingBytes() / minimum_bytes)
    throw std::invalid_argument("Invalid gift collection length.");
  return count;
}
void CheckPacket(const TcpPacket& packet, std::uint16_t message_id) {
  if (packet.message_id != message_id ||
      packet.payload.size() > kMaxGiftPayloadBytes)
    throw std::invalid_argument("Invalid gift message envelope.");
}

void WriteReceivedGiftRecord(GiftWriter& writer,
                             const ReceivedGiftRecord& value) {
  writer.WriteUInt32(value.gift_id);
  writer.WriteUInt64(value.wide_at8);
  writer.WriteUInt32(value.value_at16);
  writer.WriteUtf16(value.text_at20);
  writer.WriteUInt32(value.stock_id);
  writer.WriteUInt16(value.short_at48);
  writer.WriteUtf16(value.text_at52);
  writer.WriteUInt64(value.wide_at80);
}

ReceivedGiftRecord ReadReceivedGiftRecord(PacketReader& reader) {
  ReceivedGiftRecord value;
  value.gift_id = reader.ReadUInt32();
  value.wide_at8 = reader.ReadUInt64();
  value.value_at16 = reader.ReadUInt32();
  value.text_at20 = reader.ReadUtf16();
  value.stock_id = reader.ReadUInt32();
  value.short_at48 = reader.ReadUInt16();
  value.text_at52 = reader.ReadUtf16();
  value.wide_at80 = reader.ReadUInt64();
  return value;
}

void WriteSentGiftRecord(GiftWriter& writer, const SentGiftRecord& value) {
  writer.WriteUInt32(value.gift_id);
  writer.WriteUInt64(value.wide_at8);
  writer.WriteUInt64(value.wide_at16);
  writer.WriteUInt32(value.value_at24);
  writer.WriteUtf16(value.text_at28);
  writer.WriteUInt32(value.stock_id);
  writer.WriteUInt16(value.short_at56);
  writer.WriteUInt16(value.short_at58);
  writer.WriteUtf16(value.text_at60);
  writer.WriteUtf16(value.text_at84);
}

SentGiftRecord ReadSentGiftRecord(PacketReader& reader) {
  SentGiftRecord value;
  value.gift_id = reader.ReadUInt32();
  value.wide_at8 = reader.ReadUInt64();
  value.wide_at16 = reader.ReadUInt64();
  value.value_at24 = reader.ReadUInt32();
  value.text_at28 = reader.ReadUtf16();
  value.stock_id = reader.ReadUInt32();
  value.short_at56 = reader.ReadUInt16();
  value.short_at58 = reader.ReadUInt16();
  value.text_at60 = reader.ReadUtf16();
  value.text_at84 = reader.ReadUtf16();
  return value;
}

void WriteGiftDetailRecord(GiftWriter& writer, const GiftDetailRecord& value) {
  writer.WriteUInt32(value.value_at0);
  writer.WriteUInt32(value.value_at4);
  writer.Count(value.values_at8.size());
  for (const auto& entry : value.values_at8) {
    writer.WriteUInt32(entry);
  }
  writer.WriteUInt32(value.value_at20);
  writer.WriteUtf16(value.text_at24);
}

GiftDetailRecord ReadGiftDetailRecord(PacketReader& reader) {
  GiftDetailRecord value;
  value.value_at0 = reader.ReadUInt32();
  value.value_at4 = reader.ReadUInt32();
  {
    const auto count = ReadCount(reader, 4);
    value.values_at8.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
      value.values_at8.push_back(reader.ReadUInt32());
  }
  value.value_at20 = reader.ReadUInt32();
  value.text_at24 = reader.ReadUtf16();
  return value;
}

void WriteGiftDetails(GiftWriter& writer, const GiftDetails& value) {
  writer.WriteUInt32(value.value_at0);
  writer.Count(value.values_at4.size());
  for (const auto& entry : value.values_at4) {
    writer.WriteUInt32(entry);
  }
  writer.Count(value.values_at16.size());
  for (const auto& entry : value.values_at16) {
    writer.WriteUInt32(entry);
  }
  writer.Count(value.values_at28.size());
  for (const auto& entry : value.values_at28) {
    writer.WriteUInt32(entry);
  }
  writer.Count(value.bytes_at40.size());
  for (const auto& entry : value.bytes_at40) {
    writer.WriteUInt8(entry);
  }
  writer.Count(value.records_at52.size());
  for (const auto& entry : value.records_at52) {
    WriteGiftDetailRecord(writer, entry);
  }
}

GiftDetails ReadGiftDetails(PacketReader& reader) {
  GiftDetails value;
  value.value_at0 = reader.ReadUInt32();
  {
    const auto count = ReadCount(reader, 4);
    value.values_at4.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
      value.values_at4.push_back(reader.ReadUInt32());
  }
  {
    const auto count = ReadCount(reader, 4);
    value.values_at16.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
      value.values_at16.push_back(reader.ReadUInt32());
  }
  {
    const auto count = ReadCount(reader, 4);
    value.values_at28.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
      value.values_at28.push_back(reader.ReadUInt32());
  }
  {
    const auto count = ReadCount(reader, 1);
    value.bytes_at40.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
      value.bytes_at40.push_back(reader.ReadUInt8());
  }
  {
    const auto count = ReadCount(reader, 20);
    value.records_at52.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
      value.records_at52.push_back(ReadGiftDetailRecord(reader));
  }
  return value;
}
}  // namespace

TcpPacket GiftReceivedListRequest::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(offset);
  writer.WriteUInt32(count);
  return {kMessageId, writer.TakeBytes()};
}
GiftReceivedListRequest GiftReceivedListRequest::Decode(
    const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftReceivedListRequest value;
  value.offset = reader.ReadUInt32();
  value.count = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftSentListRequest::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(offset);
  writer.WriteUInt32(count);
  return {kMessageId, writer.TakeBytes()};
}
GiftSentListRequest GiftSentListRequest::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftSentListRequest value;
  value.offset = reader.ReadUInt32();
  value.count = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftSendRequest::Encode() const {
  GiftWriter writer;
  writer.WriteUtf16(recipient_name);
  writer.WriteUtf16(text_at24);
  writer.WriteUInt32(stock_id);
  writer.WriteUtf16(message);
  return {kMessageId, writer.TakeBytes()};
}
GiftSendRequest GiftSendRequest::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftSendRequest value;
  value.recipient_name = reader.ReadUtf16();
  value.text_at24 = reader.ReadUtf16();
  value.stock_id = reader.ReadUInt32();
  value.message = reader.ReadUtf16();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftReceiveRequest::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(gift_id);
  return {kMessageId, writer.TakeBytes()};
}
GiftReceiveRequest GiftReceiveRequest::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftReceiveRequest value;
  value.gift_id = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftDiscardRequest::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(gift_id);
  return {kMessageId, writer.TakeBytes()};
}
GiftDiscardRequest GiftDiscardRequest::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftDiscardRequest value;
  value.gift_id = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftSendThanksRequest::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(gift_id);
  writer.WriteUtf16(message);
  writer.WriteUInt32(value_at28);
  return {kMessageId, writer.TakeBytes()};
}
GiftSendThanksRequest GiftSendThanksRequest::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftSendThanksRequest value;
  value.gift_id = reader.ReadUInt32();
  value.message = reader.ReadUtf16();
  value.value_at28 = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftViewThanksRequest::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(gift_id);
  return {kMessageId, writer.TakeBytes()};
}
GiftViewThanksRequest GiftViewThanksRequest::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftViewThanksRequest value;
  value.gift_id = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftDeleteSentRequest::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(gift_id);
  return {kMessageId, writer.TakeBytes()};
}
GiftDeleteSentRequest GiftDeleteSentRequest::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftDeleteSentRequest value;
  value.gift_id = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftDeleteReceivedRequest::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(gift_id);
  return {kMessageId, writer.TakeBytes()};
}
GiftDeleteReceivedRequest GiftDeleteReceivedRequest::Decode(
    const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftDeleteReceivedRequest value;
  value.gift_id = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftNewReceiveNotification::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(value_at0);
  writer.WriteUInt32(new_count);
  return {kMessageId, writer.TakeBytes()};
}
GiftNewReceiveNotification GiftNewReceiveNotification::Decode(
    const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftNewReceiveNotification value;
  value.value_at0 = reader.ReadUInt32();
  value.new_count = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftReceivedListResponse::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(total_count);
  writer.WriteUInt32(value_at8);
  writer.Count(records.size());
  for (const auto& entry : records) {
    WriteReceivedGiftRecord(writer, entry);
  }
  return {kMessageId, writer.TakeBytes()};
}
GiftReceivedListResponse GiftReceivedListResponse::Decode(
    const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftReceivedListResponse value;
  value.result = reader.ReadUInt32();
  value.total_count = reader.ReadUInt32();
  value.value_at8 = reader.ReadUInt32();
  {
    const auto count = ReadCount(reader, 38);
    value.records.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
      value.records.push_back(ReadReceivedGiftRecord(reader));
  }
  reader.RequireEnd();
  return value;
}

TcpPacket GiftSentListResponse::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(total_count);
  writer.Count(records.size());
  for (const auto& entry : records) {
    WriteSentGiftRecord(writer, entry);
  }
  return {kMessageId, writer.TakeBytes()};
}
GiftSentListResponse GiftSentListResponse::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftSentListResponse value;
  value.result = reader.ReadUInt32();
  value.total_count = reader.ReadUInt32();
  {
    const auto count = ReadCount(reader, 44);
    value.records.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
      value.records.push_back(ReadSentGiftRecord(reader));
  }
  reader.RequireEnd();
  return value;
}

TcpPacket GiftSendResponse::Encode() const {
  GiftWriter writer;
  writer.WriteUtf16(text_at0);
  writer.WriteUInt32(value_at24);
  writer.WriteUInt32(result);
  writer.WriteUtf16(text_at32);
  writer.WriteUtf16(text_at56);
  return {kMessageId, writer.TakeBytes()};
}
GiftSendResponse GiftSendResponse::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftSendResponse value;
  value.text_at0 = reader.ReadUtf16();
  value.value_at24 = reader.ReadUInt32();
  value.result = reader.ReadUInt32();
  value.text_at32 = reader.ReadUtf16();
  value.text_at56 = reader.ReadUtf16();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftReceiveResponse::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(gift_id);
  writer.WriteUInt32(value_at8);
  writer.WriteUInt32(new_count);
  return {kMessageId, writer.TakeBytes()};
}
GiftReceiveResponse GiftReceiveResponse::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftReceiveResponse value;
  value.result = reader.ReadUInt32();
  value.gift_id = reader.ReadUInt32();
  value.value_at8 = reader.ReadUInt32();
  value.new_count = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftDiscardResponse::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(gift_id);
  writer.WriteUInt32(new_count);
  return {kMessageId, writer.TakeBytes()};
}
GiftDiscardResponse GiftDiscardResponse::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftDiscardResponse value;
  value.result = reader.ReadUInt32();
  value.gift_id = reader.ReadUInt32();
  value.new_count = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftSendThanksResponse::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(result);
  return {kMessageId, writer.TakeBytes()};
}
GiftSendThanksResponse GiftSendThanksResponse::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftSendThanksResponse value;
  value.result = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftViewThanksResponse::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(value_at0);
  writer.WriteUInt32(gift_id);
  writer.WriteUtf16(text_at8);
  writer.WriteUtf16(text_at32);
  writer.WriteUInt32(value_at56);
  writer.WriteUInt32(value_at60);
  WriteGiftDetails(writer, extra);
  return {kMessageId, writer.TakeBytes()};
}
GiftViewThanksResponse GiftViewThanksResponse::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftViewThanksResponse value;
  value.value_at0 = reader.ReadUInt32();
  value.gift_id = reader.ReadUInt32();
  value.text_at8 = reader.ReadUtf16();
  value.text_at32 = reader.ReadUtf16();
  value.value_at56 = reader.ReadUInt32();
  value.value_at60 = reader.ReadUInt32();
  value.extra = ReadGiftDetails(reader);
  reader.RequireEnd();
  return value;
}

TcpPacket GiftConfirmResponse::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(result);
  return {kMessageId, writer.TakeBytes()};
}
GiftConfirmResponse GiftConfirmResponse::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftConfirmResponse value;
  value.result = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftDeleteSentResponse::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(gift_id);
  return {kMessageId, writer.TakeBytes()};
}
GiftDeleteSentResponse GiftDeleteSentResponse::Decode(const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftDeleteSentResponse value;
  value.result = reader.ReadUInt32();
  value.gift_id = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}

TcpPacket GiftDeleteReceivedResponse::Encode() const {
  GiftWriter writer;
  writer.WriteUInt32(result);
  writer.WriteUInt32(gift_id);
  writer.WriteUInt32(new_count);
  return {kMessageId, writer.TakeBytes()};
}
GiftDeleteReceivedResponse GiftDeleteReceivedResponse::Decode(
    const TcpPacket& packet) {
  CheckPacket(packet, kMessageId);
  PacketReader reader(packet.payload);
  GiftDeleteReceivedResponse value;
  value.result = reader.ReadUInt32();
  value.gift_id = reader.ReadUInt32();
  value.new_count = reader.ReadUInt32();
  reader.RequireEnd();
  return value;
}
}  // namespace bubble_fighter::server::protocol
