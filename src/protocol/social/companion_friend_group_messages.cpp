#include "protocol/social/companion_friend_group_messages.h"

#include <set>
#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
void Header(const TcpPacket& packet, std::uint16_t id, PacketReader& reader) {
  if (packet.message_id != id || reader.ReadUInt32() != 1) {
    throw std::invalid_argument("Unsupported companion friend group protocol.");
  }
}
void Action(CompanionFriendGroupAction action) {
  if (action > CompanionFriendGroupAction::kMove) {
    throw std::invalid_argument("Unknown companion friend group action.");
  }
}
std::u16string ReadName(PacketReader& reader) {
  // Validate before allocating or reading the counted payload.
  const auto count = reader.ReadUInt32();
  if (count > 31 || count > reader.RemainingBytes() / 2) {
    throw std::invalid_argument("Invalid companion friend group name length.");
  }
  std::u16string name;
  name.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    name.push_back(reader.ReadUInt16());
  }
  return name;
}
}  // namespace

bool ValidCompanionFriendGroupName(std::u16string_view name) {
  if (name.empty() || name.size() > 31) {
    return false;
  }
  bool visible = false;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const auto value = name[i];
    if (value < 0x20 || (value >= 0x7F && value <= 0x9F) ||
        (value >= 0xDC00 && value <= 0xDFFF)) {
      return false;
    }
    if (value >= 0xD800 && value <= 0xDBFF) {
      if (++i >= name.size() || name[i] < 0xDC00 || name[i] > 0xDFFF) {
        return false;
      }
    }
    if (value != u' ' && value != 0x3000) {
      visible = true;
    }
  }
  return visible;
}

TcpPacket CompanionFriendGroupRequest::Encode() const {
  Action(action);
  bool valid = false;
  switch (action) {
    case CompanionFriendGroupAction::kList:
      valid = !group_id && !friend_id && !expected_group_id && name.empty();
      break;
    case CompanionFriendGroupAction::kCreate:
      valid = !group_id && !friend_id && !expected_group_id &&
              ValidCompanionFriendGroupName(name);
      break;
    case CompanionFriendGroupAction::kRename:
      valid = group_id && !friend_id && !expected_group_id &&
              ValidCompanionFriendGroupName(name);
      break;
    case CompanionFriendGroupAction::kRemove:
      valid = group_id && !friend_id && !expected_group_id && name.empty();
      break;
    case CompanionFriendGroupAction::kMove:
      valid = friend_id && group_id != expected_group_id && name.empty();
      break;
  }
  if (!valid) {
    throw std::invalid_argument(
        "Invalid companion friend group request shape.");
  }
  PacketWriter writer;
  writer.WriteUInt32(1);
  writer.WriteUInt8(static_cast<std::uint8_t>(action));
  writer.WriteUInt32(group_id);
  writer.WriteUInt32(friend_id);
  writer.WriteUInt32(expected_group_id);
  writer.WriteUtf16(name);
  return {kMessageId, writer.TakeBytes()};
}

CompanionFriendGroupRequest CompanionFriendGroupRequest::Decode(
    const TcpPacket& packet) {
  PacketReader reader(packet.payload);
  Header(packet, kMessageId, reader);
  CompanionFriendGroupRequest result;
  result.action = static_cast<CompanionFriendGroupAction>(reader.ReadUInt8());
  result.group_id = reader.ReadUInt32();
  result.friend_id = reader.ReadUInt32();
  result.expected_group_id = reader.ReadUInt32();
  result.name = ReadName(reader);
  reader.RequireEnd();
  (void)result.Encode();
  return result;
}

TcpPacket CompanionFriendGroupsNotification::Encode() const {
  Action(action);
  if (status > CompanionFriendGroupStatus::kUnavailable ||
      groups.size() > kMaximumGroups ||
      assignments.size() > kMaximumAssignments) {
    throw std::invalid_argument("Invalid companion friend group snapshot.");
  }
  PacketWriter writer;
  writer.WriteUInt32(1);
  writer.WriteUInt32(static_cast<std::uint32_t>(status));
  writer.WriteUInt8(static_cast<std::uint8_t>(action));
  writer.WriteUInt32(static_cast<std::uint32_t>(groups.size()));
  std::set<std::uint32_t> ids;
  std::set<std::u16string_view> names;
  for (const auto& group : groups) {
    if (!group.group_id || !ValidCompanionFriendGroupName(group.name) ||
        !ids.insert(group.group_id).second ||
        !names.insert(group.name).second) {
      throw std::invalid_argument("Invalid companion friend group identity.");
    }
    writer.WriteUInt32(group.group_id);
    writer.WriteUtf16(group.name);
  }
  writer.WriteUInt32(static_cast<std::uint32_t>(assignments.size()));
  std::set<std::uint32_t> friends;
  for (const auto& assignment : assignments) {
    if (!assignment.friend_id || !ids.contains(assignment.group_id) ||
        !friends.insert(assignment.friend_id).second) {
      throw std::invalid_argument("Invalid companion friend group assignment.");
    }
    writer.WriteUInt32(assignment.friend_id);
    writer.WriteUInt32(assignment.group_id);
  }
  return {kMessageId, writer.TakeBytes()};
}

CompanionFriendGroupsNotification CompanionFriendGroupsNotification::Decode(
    const TcpPacket& packet) {
  PacketReader reader(packet.payload);
  Header(packet, kMessageId, reader);
  CompanionFriendGroupsNotification result;
  result.status = static_cast<CompanionFriendGroupStatus>(reader.ReadUInt32());
  result.action = static_cast<CompanionFriendGroupAction>(reader.ReadUInt8());
  const auto groups = reader.ReadUInt32();
  if (groups > kMaximumGroups || groups > reader.RemainingBytes() / 10) {
    throw std::invalid_argument("Companion friend group capacity exceeded.");
  }
  for (std::uint32_t i = 0; i < groups; ++i) {
    const auto id = reader.ReadUInt32();
    result.groups.push_back({id, ReadName(reader)});
  }
  const auto assignments = reader.ReadUInt32();
  if (assignments > kMaximumAssignments ||
      assignments > reader.RemainingBytes() / 8) {
    throw std::invalid_argument(
        "Companion friend group assignment capacity exceeded.");
  }
  for (std::uint32_t i = 0; i < assignments; ++i) {
    const auto friend_id = reader.ReadUInt32();
    result.assignments.push_back({friend_id, reader.ReadUInt32()});
  }
  reader.RequireEnd();
  (void)result.Encode();
  return result;
}
}  // namespace bubble_fighter::server::protocol
