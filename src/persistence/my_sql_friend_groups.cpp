#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_social_store.h"
#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
using namespace features::social;
using namespace protocol;
namespace {
using Action = CompanionFriendGroupAction;
using Status = CompanionFriendGroupStatus;
std::uint32_t GroupIdentity(std::uint64_t id) {
  if (!id || id > UINT32_MAX) {
    throw std::runtime_error("Friend group identity exceeds client range.");
  }
  return static_cast<std::uint32_t>(id);
}
std::u16string GroupName(std::span<const std::uint8_t> bytes) {
  if (bytes.size() % 2) {
    throw std::runtime_error("Invalid stored friend group name.");
  }
  PacketReader reader(bytes);
  std::u16string name;
  while (reader.RemainingBytes()) {
    name.push_back(reader.ReadUInt16());
  }
  if (!ValidCompanionFriendGroupName(name)) {
    throw std::runtime_error("Invalid stored friend group name.");
  }
  return name;
}
std::vector<std::uint8_t> GroupNameBytes(std::u16string_view name) {
  PacketWriter writer;
  for (auto unit : name) {
    writer.WriteUInt16(unit);
  }
  return writer.TakeBytes();
}
CompanionFriendGroupsNotification GroupView(const SocialSnapshot& snapshot,
                                            Status status = Status::kOk,
                                            Action action = Action::kList) {
  return {status, action, snapshot.groups, snapshot.assignments};
}
}  // namespace

void MySqlSocialStore::ReadGroups(SocialSnapshot& snapshot) {
  const auto owner = snapshot.owner.character_id;
  {
    Statement query(
        connection_.Handle(),
        "SELECT GroupId, NameUtf16 FROM SocialFriendGroups WHERE OwnerCharacterId = ? ORDER BY GroupId");
    std::array parameters{Number(owner)};
    query.Execute(parameters);
    std::uint64_t id = 0;
    std::array<std::uint8_t, 62> raw{};
    unsigned long length = 0;
    std::array result{
        Number(id), Bytes(raw.data(), static_cast<unsigned long>(raw.size()))};
    result[1].length = &length;
    while (query.Fetch(result)) {
      if (snapshot.groups.size() >=
          CompanionFriendGroupsNotification::kMaximumGroups) {
        throw std::runtime_error("Stored friend group capacity exceeded.");
      }
      snapshot.groups.push_back(
          {GroupIdentity(id), GroupName(std::span(raw).first(length))});
    }
  }
  {
    Statement query(
        connection_.Handle(),
        "SELECT g.FriendCharacterId, g.GroupId, f.IsAccepted FROM SocialFriendGroupAssignments g JOIN SocialFriendships f ON f.FirstCharacterId = g.FirstCharacterId AND f.SecondCharacterId = g.SecondCharacterId WHERE g.OwnerCharacterId = ? ORDER BY g.FriendCharacterId");
    std::array parameters{Number(owner)};
    query.Execute(parameters);
    std::uint64_t id = 0, group = 0;
    std::uint8_t accepted = 0;
    std::array result{Number(id), Number(group), Number(accepted)};
    while (query.Fetch(result)) {
      const auto friend_id = GroupIdentity(id);
      if (accepted != 1 ||
          snapshot.assignments.size() >=
              CompanionFriendGroupsNotification::kMaximumAssignments ||
          std::none_of(
              snapshot.rows.begin(), snapshot.rows.end(), [&](const auto& row) {
                return row.character_id == friend_id &&
                       (row.relationship == 1 || row.relationship == 4);
              })) {
        throw std::runtime_error("Invalid stored friend group relationship.");
      }
      snapshot.assignments.push_back({friend_id, GroupIdentity(group)});
    }
  }
  (void)GroupView(snapshot).Encode();
}

SocialGroupChange MySqlSocialStore::ChangeGroup(
    const features::characters::CharacterRecord& owner,
    const CompanionFriendGroupRequest& request) {
  (void)request.Encode();
  const auto own_id = GroupIdentity(owner.character_id);
  const bool move = request.action == Action::kMove;
  // Resolve existence before choosing the ordered lock set. Never lock the
  // owner first and then a smaller peer; friend changes use the same sorted
  // pair locks.
  const auto peer = move && request.friend_id != own_id
                        ? Find(request.friend_id, {})
                        : std::nullopt;
  Transaction tx(connection_.Handle());
  LockPair(owner, peer ? peer->character_id : own_id);
  auto snapshot = LoadLocked(owner);
  const auto operation = [&]() -> Status {
    if (request.action == Action::kList) {
      return Status::kOk;
    }
    const auto target = std::find_if(
        snapshot.groups.begin(), snapshot.groups.end(),
        [&](const auto& group) { return group.group_id == request.group_id; });
    if ((request.action == Action::kRename ||
         request.action == Action::kRemove || (move && request.group_id)) &&
        target == snapshot.groups.end()) {
      return Status::kNotFound;
    }
    if (request.action == Action::kCreate ||
        request.action == Action::kRename) {
      if (std::any_of(snapshot.groups.begin(), snapshot.groups.end(),
                      [&](const auto& group) {
                        return group.name == request.name &&
                               group.group_id != request.group_id;
                      })) {
        return Status::kDuplicate;
      }
      const auto bytes = GroupNameBytes(request.name);
      if (request.action == Action::kCreate) {
        if (snapshot.groups.size() >=
            CompanionFriendGroupsNotification::kMaximumGroups) {
          return Status::kCapacity;
        }
        Statement query(
            connection_.Handle(),
            "INSERT INTO SocialFriendGroups (OwnerCharacterId, NameUtf16) VALUES (?, ?)");
        std::array parameters{
            Number(owner.character_id),
            Bytes(bytes.data(), static_cast<unsigned long>(bytes.size()))};
        query.Execute(parameters);
      } else {
        Statement query(
            connection_.Handle(),
            "UPDATE SocialFriendGroups SET NameUtf16 = ? WHERE OwnerCharacterId = ? AND GroupId = ?");
        std::array parameters{
            Bytes(bytes.data(), static_cast<unsigned long>(bytes.size())),
            Number(owner.character_id), Number(request.group_id)};
        query.Execute(parameters);
      }
    } else if (request.action == Action::kRemove) {
      if (std::any_of(snapshot.assignments.begin(), snapshot.assignments.end(),
                      [&](const auto& row) {
                        return row.group_id == request.group_id;
                      })) {
        return Status::kNotEmpty;
      }
      Statement query(
          connection_.Handle(),
          "DELETE FROM SocialFriendGroups WHERE OwnerCharacterId = ? AND GroupId = ?");
      std::array parameters{Number(owner.character_id),
                            Number(request.group_id)};
      query.Execute(parameters);
    } else {
      const auto first = (std::min)(own_id, request.friend_id);
      const auto second = (std::max)(own_id, request.friend_id);
      bool accepted = false;
      if (peer) {
        Statement query(
            connection_.Handle(),
            "SELECT IsAccepted FROM SocialFriendships WHERE FirstCharacterId = ? AND SecondCharacterId = ?");
        std::array parameters{Number(first), Number(second)};
        query.Execute(parameters);
        std::uint8_t value = 0;
        std::array result{Number(value)};
        accepted = query.Fetch(result) && value == 1;
      }
      if (!accepted) {
        return Status::kNotFriend;
      }
      const auto old = std::find_if(
          snapshot.assignments.begin(), snapshot.assignments.end(),
          [&](const auto& row) { return row.friend_id == request.friend_id; });
      const auto current =
          old == snapshot.assignments.end() ? 0 : old->group_id;
      if (current != request.expected_group_id) {
        return Status::kStale;
      }
      if (old != snapshot.assignments.end()) {
        Statement query(
            connection_.Handle(),
            "DELETE FROM SocialFriendGroupAssignments WHERE OwnerCharacterId = ? AND FriendCharacterId = ?");
        std::array parameters{Number(owner.character_id),
                              Number(request.friend_id)};
        query.Execute(parameters);
      }
      if (request.group_id) {
        Statement query(
            connection_.Handle(),
            "INSERT INTO SocialFriendGroupAssignments (OwnerCharacterId, FriendCharacterId, GroupId, FirstCharacterId, SecondCharacterId) VALUES (?, ?, ?, ?, ?)");
        std::array parameters{
            Number(owner.character_id), Number(request.friend_id),
            Number(request.group_id), Number(first), Number(second)};
        query.Execute(parameters);
      }
    }
    snapshot = LoadLocked(owner);
    return Status::kOk;
  };
  const auto status = operation();
  // Validate the full reply before committing, including new AUTO_INCREMENT
  // IDs.
  (void)GroupView(snapshot, status, request.action).Encode();
  tx.Commit();
  return {status, std::move(snapshot)};
}
}  // namespace bubble_fighter::server::persistence
