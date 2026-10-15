#include "protocol/social/companion_mailbox_messages.h"

#include <stdexcept>
#include <string_view>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
void Header(const TcpPacket& packet, std::uint16_t id, PacketReader& reader) {
  if (packet.message_id != id || reader.ReadUInt32() != 1) {
    throw std::invalid_argument("Unsupported companion mailbox protocol.");
  }
}
void Text(std::u16string_view text, std::size_t maximum) {
  if (text.empty() || text.size() > maximum) {
    throw std::invalid_argument("Invalid mailbox text.");
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = text[i];
    if (c < 0x20 || c == 0x7f || (c >= 0xdc00 && c <= 0xdfff)) {
      throw std::invalid_argument("Invalid mailbox text.");
    }
    if (c >= 0xd800 && c <= 0xdbff) {
      if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff) {
        throw std::invalid_argument("Invalid mailbox text.");
      }
    }
  }
}
std::u16string ReadText(PacketReader& reader, std::size_t maximum) {
  const auto count = reader.ReadUInt32();
  if (!count || count > maximum || count > reader.RemainingBytes() / 2) {
    throw std::invalid_argument("Invalid mailbox text.");
  }
  std::u16string result;
  result.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    result.push_back(reader.ReadUInt16());
  }
  Text(result, maximum);
  return result;
}
}  // namespace
TcpPacket CompanionMailboxRequest::Encode() const {
  if (action > CompanionMailboxAction::kAcknowledge ||
      (action == CompanionMailboxAction::kFetch ? message_id != 0
                                                : message_id == 0)) {
    throw std::invalid_argument("Invalid mailbox request.");
  }
  PacketWriter writer;
  writer.WriteUInt32(1);
  writer.WriteUInt8(static_cast<std::uint8_t>(action));
  writer.WriteUInt64(message_id);
  return {kMessageId, writer.TakeBytes()};
}
CompanionMailboxRequest CompanionMailboxRequest::Decode(
    const TcpPacket& packet) {
  PacketReader reader(packet.payload);
  Header(packet, kMessageId, reader);
  CompanionMailboxRequest result{
      static_cast<CompanionMailboxAction>(reader.ReadUInt8()),
      reader.ReadUInt64()};
  reader.RequireEnd();
  (void)result.Encode();
  return result;
}
TcpPacket CompanionMailboxNotification::Encode() const {
  if (status > CompanionMailboxStatus::kRateLimited ||
      messages.size() > kMaximumMessages ||
      (status != CompanionMailboxStatus::kOk && !messages.empty())) {
    throw std::invalid_argument("Invalid mailbox batch.");
  }
  PacketWriter writer;
  writer.WriteUInt32(1);
  writer.WriteUInt32(static_cast<std::uint32_t>(status));
  writer.WriteUInt32(static_cast<std::uint32_t>(messages.size()));
  std::uint64_t previous = 0;
  for (const auto& message : messages) {
    if (message.message_id <= previous || !message.sender_id ||
        !message.sent_at_unix_seconds ||
        message.sent_at_unix_seconds > kMaximumTimestamp) {
      throw std::invalid_argument("Invalid mailbox message identity.");
    }
    Text(message.sender_name, 32);
    Text(message.text, 256);
    writer.WriteUInt64(message.message_id);
    writer.WriteUInt32(message.sender_id);
    writer.WriteUInt64(message.sent_at_unix_seconds);
    writer.WriteUtf16(message.sender_name);
    writer.WriteUtf16(message.text);
    previous = message.message_id;
  }
  return {kMessageId, writer.TakeBytes()};
}
CompanionMailboxNotification CompanionMailboxNotification::Decode(
    const TcpPacket& packet) {
  PacketReader reader(packet.payload);
  Header(packet, kMessageId, reader);
  CompanionMailboxNotification result;
  result.status = static_cast<CompanionMailboxStatus>(reader.ReadUInt32());
  const auto count = reader.ReadUInt32();
  if (result.status > CompanionMailboxStatus::kRateLimited ||
      count > kMaximumMessages ||
      (result.status != CompanionMailboxStatus::kOk && count)) {
    throw std::invalid_argument("Invalid mailbox batch.");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    CompanionQueuedMessage message;
    message.message_id = reader.ReadUInt64();
    message.sender_id = reader.ReadUInt32();
    message.sent_at_unix_seconds = reader.ReadUInt64();
    message.sender_name = ReadText(reader, 32);
    message.text = ReadText(reader, 256);
    result.messages.push_back(std::move(message));
  }
  reader.RequireEnd();
  (void)result.Encode();
  return result;
}
}  // namespace bubble_fighter::server::protocol
