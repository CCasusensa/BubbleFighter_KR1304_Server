#include "protocol/social/companion_social_messages.h"

#include <set>
#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"
namespace bubble_fighter::server::protocol {
namespace {
void Header(const TcpPacket& p, std::uint16_t id, PacketReader& r) {
  if (p.message_id != id || r.ReadUInt32() != 1) {
    throw std::invalid_argument("Unsupported companion social protocol.");
  }
}
void ValidateAction(CompanionSocialAction a) {
  if (a > CompanionSocialAction::kMessage) {
    throw std::invalid_argument("Unknown companion social action.");
  }
}
void ValidateText(std::u16string_view s, std::size_t maximum) {
  if (s.size() > maximum || s.find(u'\0') != s.npos) {
    throw std::invalid_argument("Invalid companion social text length.");
  }
}
}  // namespace
TcpPacket CompanionSocialRequest::Encode() const {
  ValidateAction(action);
  ValidateText(target_name, 32);
  ValidateText(text, 256);
  PacketWriter w;
  w.WriteUInt32(1);
  w.WriteUInt8(static_cast<std::uint8_t>(this->action));
  w.WriteUInt32(target_id);
  w.WriteUtf16(target_name);
  w.WriteUtf16(this->text);
  return {kMessageId, w.TakeBytes()};
}
CompanionSocialRequest CompanionSocialRequest::Decode(const TcpPacket& p) {
  PacketReader r(p.payload);
  Header(p, kMessageId, r);
  CompanionSocialRequest m;
  m.action = static_cast<CompanionSocialAction>(r.ReadUInt8());
  m.target_id = r.ReadUInt32();
  m.target_name = r.ReadUtf16();
  m.text = r.ReadUtf16();
  r.RequireEnd();
  (void)m.Encode();
  return m;
}
TcpPacket CompanionSocialRosterNotification::Encode() const {
  ValidateAction(action);
  ValidateText(notice, 256);
  if (rows.size() > 250 || status > CompanionSocialStatus::kUnavailable) {
    throw std::invalid_argument("Invalid social roster.");
  }
  PacketWriter w;
  w.WriteUInt32(1);
  w.WriteUInt32(static_cast<std::uint32_t>(status));
  w.WriteUInt8(static_cast<std::uint8_t>(this->action));
  w.WriteUtf16(notice);
  w.WriteUInt32(static_cast<std::uint32_t>(rows.size()));
  std::set<std::uint32_t> ids;
  for (const auto& row : rows) {
    ValidateText(row.name, 32);
    if (!row.character_id || row.name.empty() || row.relationship > 4 ||
        !ids.insert(row.character_id).second) {
      throw std::invalid_argument("Invalid social roster identity.");
    }
    w.WriteUInt32(row.character_id);
    w.WriteUtf16(row.name);
    w.WriteUInt8(row.relationship);
    w.WriteUInt8(row.online ? 1 : 0);
  }
  return {kMessageId, w.TakeBytes()};
}
CompanionSocialRosterNotification CompanionSocialRosterNotification::Decode(
    const TcpPacket& p) {
  PacketReader r(p.payload);
  Header(p, kMessageId, r);
  CompanionSocialRosterNotification m;
  m.status = static_cast<CompanionSocialStatus>(r.ReadUInt32());
  m.action = static_cast<CompanionSocialAction>(r.ReadUInt8());
  m.notice = r.ReadUtf16();
  const auto count = r.ReadUInt32();
  if (count > 250) {
    throw std::invalid_argument("Social roster capacity exceeded.");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    CompanionSocialRow row;
    row.character_id = r.ReadUInt32();
    row.name = r.ReadUtf16();
    row.relationship = r.ReadUInt8();
    const auto online = r.ReadUInt8();
    if (online > 1) {
      throw std::invalid_argument("Invalid presence flag.");
    }
    row.online = online != 0;
    m.rows.push_back(std::move(row));
  }
  r.RequireEnd();
  (void)m.Encode();
  return m;
}
TcpPacket CompanionPrivateMessageNotification::Encode() const {
  ValidateText(peer_name, 32);
  ValidateText(speaker, 32);
  ValidateText(text, 256);
  if (!peer_id || peer_name.empty() || speaker.empty() || this->text.empty()) {
    throw std::invalid_argument("Invalid private message.");
  }
  PacketWriter w;
  w.WriteUInt32(1);
  w.WriteUInt32(peer_id);
  w.WriteUtf16(peer_name);
  w.WriteUtf16(speaker);
  w.WriteUtf16(this->text);
  return {kMessageId, w.TakeBytes()};
}
CompanionPrivateMessageNotification CompanionPrivateMessageNotification::Decode(
    const TcpPacket& p) {
  PacketReader r(p.payload);
  Header(p, kMessageId, r);
  CompanionPrivateMessageNotification m{r.ReadUInt32(), r.ReadUtf16(),
                                        r.ReadUtf16(), r.ReadUtf16()};
  r.RequireEnd();
  (void)m.Encode();
  return m;
}
}  // namespace bubble_fighter::server::protocol
