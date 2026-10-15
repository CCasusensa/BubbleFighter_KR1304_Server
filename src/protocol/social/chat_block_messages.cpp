#include "protocol/social/chat_block_messages.h"

#include <set>
#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
void Validate(std::u16string_view name, std::optional<std::uint32_t> id) {
  if (name.empty() || name.size() > 32 || name.find(u'\0') != name.npos ||
      (id && !*id)) {
    throw std::invalid_argument("Invalid chat block identity.");
  }
}
void WriteTarget(PacketWriter& w, std::optional<std::uint32_t> id,
                 std::u16string_view name) {
  Validate(name, id);
  w.WriteUInt8(id ? 0x3A : 0xBA);
  if (id) {
    w.WriteUInt32(*id);
  }
  w.WriteUtf16(name);
}
void ReadTarget(PacketReader& r, std::optional<std::uint32_t>& id,
                std::u16string& name) {
  const auto marker = r.ReadUInt8();
  if (marker != 0x3A && marker != 0xBA) {
    throw std::invalid_argument("Unknown chat block marker.");
  }
  if (marker == 0x3A) {
    id = r.ReadUInt32();
  }
  name = r.ReadUtf16();
  Validate(name, id);
}
void RequireId(const TcpPacket& packet, std::uint16_t id) {
  if (packet.message_id != id) {
    throw std::invalid_argument("Unexpected chat block message.");
  }
}
}  // namespace
TcpPacket ChatBlockRequest::Encode() const {
  PacketWriter w;
  WriteTarget(w, character_id, name);
  return {static_cast<std::uint16_t>(remove ? 148 : 147), w.TakeBytes()};
}
ChatBlockRequest ChatBlockRequest::Decode(const TcpPacket& packet) {
  if (packet.message_id != 147 && packet.message_id != 148) {
    throw std::invalid_argument("Invalid chat block request.");
  }
  PacketReader r(packet.payload);
  ChatBlockRequest m;
  m.remove = packet.message_id == 148;
  ReadTarget(r, m.character_id, m.name);
  r.RequireEnd();
  return m;
}
TcpPacket ChatBlockListNotification::Encode() const {
  if (entries.size() > 50) {
    throw std::invalid_argument("Chat block capacity exceeded.");
  }
  PacketWriter w;
  w.WriteUInt32(static_cast<std::uint32_t>(entries.size()));
  std::set<std::uint32_t> ids;
  for (const auto& entry : entries) {
    Validate(entry.name, entry.character_id);
    if (!ids.insert(entry.character_id).second) {
      throw std::invalid_argument("Duplicate blocked identity.");
    }
    w.WriteUInt32(entry.character_id);
    w.WriteUtf16(entry.name);
  }
  return {kMessageId, w.TakeBytes()};
}
ChatBlockListNotification ChatBlockListNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader r(packet.payload);
  const auto count = r.ReadUInt32();
  if (count > 50) {
    throw std::invalid_argument("Chat block capacity exceeded.");
  }
  ChatBlockListNotification m;
  std::set<std::uint32_t> ids;
  for (std::uint32_t i = 0; i < count; ++i) {
    ChatBlockEntry e{r.ReadUInt32(), r.ReadUtf16()};
    Validate(e.name, e.character_id);
    if (!ids.insert(e.character_id).second) {
      throw std::invalid_argument("Duplicate blocked identity.");
    }
    m.entries.push_back(std::move(e));
  }
  r.RequireEnd();
  return m;
}
TcpPacket ChatBlockAddedNotification::Encode() const {
  PacketWriter w;
  WriteTarget(w, character_id, name);
  return {kMessageId, w.TakeBytes()};
}
ChatBlockAddedNotification ChatBlockAddedNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader r(packet.payload);
  ChatBlockAddedNotification m;
  ReadTarget(r, m.character_id, m.name);
  r.RequireEnd();
  return m;
}
TcpPacket ChatBlockRemovedNotification::Encode() const {
  Validate(entry.name, entry.character_id);
  PacketWriter w;
  w.WriteUInt32(entry.character_id);
  w.WriteUtf16(entry.name);
  return {kMessageId, w.TakeBytes()};
}
ChatBlockRemovedNotification ChatBlockRemovedNotification::Decode(
    const TcpPacket& packet) {
  RequireId(packet, kMessageId);
  PacketReader r(packet.payload);
  ChatBlockRemovedNotification m{{r.ReadUInt32(), r.ReadUtf16()}};
  Validate(m.entry.name, m.entry.character_id);
  r.RequireEnd();
  return m;
}
}  // namespace bubble_fighter::server::protocol
