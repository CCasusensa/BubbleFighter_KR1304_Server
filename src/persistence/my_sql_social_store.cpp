#include "persistence/my_sql_social_store.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>

#include "features/characters/character_creation.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_commands.h"
#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
using namespace features::social;
using namespace features::characters;
using protocol::CompanionSocialAction;
using protocol::CompanionSocialStatus;
namespace {
std::vector<std::uint8_t> NameBytes(std::u16string_view name) {
  protocol::PacketWriter w;
  for (auto c : name) {
    w.WriteUInt16(c);
  }
  return w.TakeBytes();
}
std::u16string Name(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || bytes.size() > 32 || bytes.size() % 2) {
    throw std::runtime_error("Invalid social character name.");
  }
  protocol::PacketReader r(bytes);
  std::u16string name;
  while (r.RemainingBytes()) {
    name.push_back(r.ReadUInt16());
  }
  if (CharacterCreation::ValidateName(name)) {
    throw std::runtime_error("Invalid stored social name.");
  }
  return name;
}
std::uint32_t Id(std::uint64_t id) {
  if (!id || id > UINT32_MAX) {
    throw std::runtime_error("Social identity exceeds client range.");
  }
  return static_cast<std::uint32_t>(id);
}
}  // namespace
MySqlSocialStore::MySqlSocialStore(const std::filesystem::path& configuration)
    : connection_(configuration) {
  {
    Statement friendships(
        connection_.Handle(),
        "SELECT FirstCharacterId, SecondCharacterId, RequestedBy, IsAccepted FROM SocialFriendships WHERE 1 = 0");
    friendships.Execute({});
  }
  {
    Statement blocks(
        connection_.Handle(),
        "SELECT OwnerCharacterId, TargetCharacterId FROM SocialChatBlocks WHERE 1 = 0");
    blocks.Execute({});
  }
  {
    Statement groups(
        connection_.Handle(),
        "SELECT GroupId, OwnerCharacterId, NameUtf16 FROM SocialFriendGroups WHERE 1 = 0");
    groups.Execute({});
  }
  {
    Statement assignments(
        connection_.Handle(),
        "SELECT OwnerCharacterId, FriendCharacterId, GroupId, FirstCharacterId, SecondCharacterId FROM SocialFriendGroupAssignments WHERE 1 = 0");
    assignments.Execute({});
  }
  {
    Statement messages(
        connection_.Handle(),
        "SELECT MessageId, SenderCharacterId, RecipientCharacterId, FirstCharacterId, SecondCharacterId, SentAtUnixSeconds, TextUtf16 FROM SocialMessages WHERE 1 = 0");
    messages.Execute({});
  }
}
std::optional<protocol::ChatBlockEntry> MySqlSocialStore::Find(
    std::uint32_t id, std::u16string_view name) {
  if (!id && CharacterCreation::ValidateName(name)) {
    return {};
  }
  const auto bytes = NameBytes(name);
  Statement q(
      connection_.Handle(),
      id ? "SELECT CharacterId, NameUtf16 FROM Characters WHERE CharacterId = ?"
         : "SELECT CharacterId, NameUtf16 FROM Characters WHERE NameUtf16 = ?");
  std::array params{
      id ? Number(id)
         : Bytes(bytes.data(), static_cast<unsigned long>(bytes.size()))};
  q.Execute(params);
  std::uint64_t found = 0;
  std::array<std::uint8_t, 32> raw{};
  unsigned long length = 0;
  std::array result{Number(found),
                    Bytes(raw.data(), static_cast<unsigned long>(raw.size()))};
  result[1].length = &length;
  if (!q.Fetch(result)) {
    return {};
  }
  auto current_name = Name(std::span(raw).first(length));
  if (id && !name.empty() && current_name != name) {
    return {};
  }
  return protocol::ChatBlockEntry{Id(found), std::move(current_name)};
}
void MySqlSocialStore::LockPair(const CharacterRecord& owner,
                                std::uint32_t target) {
  Statement q(
      connection_.Handle(),
      "SELECT c.CharacterId, c.AccountId, a.IsDisabled FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId WHERE c.CharacterId IN (?, ?) ORDER BY c.CharacterId FOR UPDATE");
  std::array params{Number(owner.character_id), Number(target)};
  q.Execute(params);
  std::uint64_t id = 0, account = 0;
  std::uint8_t disabled = 0;
  std::array result{Number(id), Number(account), Number(disabled)};
  bool self = false, peer = false;
  while (q.Fetch(result)) {
    if (disabled) {
      throw std::runtime_error("Social account is disabled.");
    }
    if (id == owner.character_id && account == owner.account_id) {
      self = true;
    }
    if (id == target) {
      peer = true;
    }
  }
  if (!self || !peer) {
    throw std::runtime_error("Social owner or target is unavailable.");
  }
}
bool MySqlSocialStore::Blocked(std::uint64_t a, std::uint64_t b) {
  Statement q(
      connection_.Handle(),
      "SELECT COUNT(*) FROM SocialChatBlocks WHERE (OwnerCharacterId = ? AND TargetCharacterId = ?) OR (OwnerCharacterId = ? AND TargetCharacterId = ?)");
  std::array params{Number(a), Number(b), Number(b), Number(a)};
  q.Execute(params);
  std::uint64_t count = 0;
  std::array result{Number(count)};
  if (!q.Fetch(result)) {
    throw std::runtime_error("Missing block count.");
  }
  return count != 0;
}
std::uint64_t MySqlSocialStore::RelationshipCount(std::uint64_t id) {
  Statement q(
      connection_.Handle(),
      "SELECT COUNT(*) FROM SocialFriendships WHERE FirstCharacterId = ? OR SecondCharacterId = ?");
  std::array params{Number(id), Number(id)};
  q.Execute(params);
  std::uint64_t count = 0;
  std::array result{Number(count)};
  if (!q.Fetch(result)) {
    throw std::runtime_error("Missing social capacity count.");
  }
  return count;
}
SocialSnapshot MySqlSocialStore::Load(const CharacterRecord& owner) {
  Transaction tx(connection_.Handle());
  LockPair(owner, Id(owner.character_id));
  auto snapshot = LoadLocked(owner);
  tx.Commit();
  return snapshot;
}
SocialSnapshot MySqlSocialStore::LoadLocked(const CharacterRecord& owner) {
  SocialSnapshot snapshot;
  snapshot.owner = owner;
  const auto current = Find(Id(owner.character_id), {});
  if (!current) {
    throw std::runtime_error("Social owner disappeared.");
  }
  snapshot.owner.character_name = current->name;
  std::map<std::uint32_t, protocol::CompanionSocialRow> rows;
  {
    Statement q(
        connection_.Handle(),
        "SELECT c.CharacterId, c.NameUtf16, f.RequestedBy, f.IsAccepted FROM SocialFriendships f JOIN Characters c ON c.CharacterId = CASE WHEN f.FirstCharacterId = ? THEN f.SecondCharacterId ELSE f.FirstCharacterId END WHERE f.FirstCharacterId = ? OR f.SecondCharacterId = ? ORDER BY c.CharacterId");
    std::array params{Number(owner.character_id), Number(owner.character_id),
                      Number(owner.character_id)};
    q.Execute(params);
    std::uint64_t id = 0, requester = 0;
    std::uint8_t accepted = 0;
    std::array<std::uint8_t, 32> raw{};
    unsigned long length = 0;
    std::array result{Number(id),
                      Bytes(raw.data(), static_cast<unsigned long>(raw.size())),
                      Number(requester), Number(accepted)};
    result[1].length = &length;
    while (q.Fetch(result)) {
      if (rows.size() >= 200 || accepted > 1) {
        throw std::runtime_error("Invalid stored friendship state.");
      }
      rows.emplace(
          Id(id),
          protocol::CompanionSocialRow{
              Id(id), Name(std::span(raw).first(length)),
              static_cast<std::uint8_t>(accepted                          ? 1
                                        : requester == owner.character_id ? 3
                                                                          : 2),
              false});
    }
  }
  {
    Statement q(
        connection_.Handle(),
        "SELECT c.CharacterId, c.NameUtf16 FROM SocialChatBlocks b JOIN Characters c ON c.CharacterId = b.TargetCharacterId WHERE b.OwnerCharacterId = ? ORDER BY c.CharacterId");
    std::array params{Number(owner.character_id)};
    q.Execute(params);
    std::uint64_t id = 0;
    std::array<std::uint8_t, 32> raw{};
    unsigned long length = 0;
    std::array result{
        Number(id), Bytes(raw.data(), static_cast<unsigned long>(raw.size()))};
    result[1].length = &length;
    while (q.Fetch(result)) {
      if (snapshot.blocks.size() >= 50) {
        throw std::runtime_error("Invalid stored block capacity.");
      }
      const auto name = Name(std::span(raw).first(length));
      snapshot.blocks.push_back({Id(id), name});
      rows[Id(id)] = {Id(id), name, 4, false};
    }
  }
  for (auto& [id, row] : rows) {
    (void)id;
    snapshot.rows.push_back(std::move(row));
  }
  ReadGroups(snapshot);
  (void)protocol::CompanionSocialRosterNotification{
      CompanionSocialStatus::kOk,
      protocol::CompanionSocialAction::kList,
      {},
      snapshot.rows}
      .Encode();
  return snapshot;
}
SocialChange MySqlSocialStore::Change(
    const CharacterRecord& owner,
    const protocol::CompanionSocialRequest& request) {
  auto target = Find(request.target_id, request.target_name);
  if (!target) {
    return {CompanionSocialStatus::kNotFound, {}};
  }
  if (target->character_id == owner.character_id) {
    return {CompanionSocialStatus::kSelf, target};
  }
  const auto a = (std::min)(owner.character_id,
                            static_cast<std::uint64_t>(target->character_id));
  const auto b = (std::max)(owner.character_id,
                            static_cast<std::uint64_t>(target->character_id));
  Transaction tx(connection_.Handle());
  LockPair(owner, target->character_id);
  // Recheck the name after acquiring both character locks (rename races).
  target = Find(target->character_id, request.target_name);
  if (!target) {
    return {CompanionSocialStatus::kNotFound, {}};
  }
  std::uint64_t requester = 0;
  std::uint8_t accepted = 0;
  {
    Statement q(
        connection_.Handle(),
        "SELECT RequestedBy, IsAccepted FROM SocialFriendships WHERE FirstCharacterId = ? AND SecondCharacterId = ?");
    std::array params{Number(a), Number(b)};
    q.Execute(params);
    std::array result{Number(requester), Number(accepted)};
    (void)q.Fetch(result);
  }
  auto pair_write = [&](std::string_view sql) {
    Statement q(connection_.Handle(), sql);
    std::array params{Number(a), Number(b)};
    q.Execute(params);
  };
  const auto action = request.action;
  if (action == CompanionSocialAction::kBlock ||
      action == CompanionSocialAction::kUnblock) {
    if (action == CompanionSocialAction::kBlock) {
      std::uint64_t count = 0, exists = 0;
      {
        Statement q(
            connection_.Handle(),
            "SELECT COUNT(*), COALESCE(SUM(TargetCharacterId = ?), 0) FROM SocialChatBlocks WHERE OwnerCharacterId = ?");
        std::array params{Number(target->character_id),
                          Number(owner.character_id)};
        q.Execute(params);
        std::array result{Number(count), Number(exists)};
        if (!q.Fetch(result)) {
          throw std::runtime_error("Missing block capacity.");
        }
      }
      if (!exists && count >= 50) {
        return {CompanionSocialStatus::kCapacity, target};
      }
      if (!exists) {
        Statement q(
            connection_.Handle(),
            "INSERT INTO SocialChatBlocks (OwnerCharacterId, TargetCharacterId) VALUES (?, ?)");
        std::array params{Number(owner.character_id),
                          Number(target->character_id)};
        q.Execute(params);
      }
      // Pending requests are cancelled; an existing friendship is retained
      // while muted.
      // Pending chat in both directions is revoked atomically with the block.
      pair_write(
          "DELETE FROM SocialMessages WHERE FirstCharacterId = ? AND SecondCharacterId = ?");
      if (requester && !accepted) {
        pair_write(
            "DELETE FROM SocialFriendships WHERE FirstCharacterId = ? AND SecondCharacterId = ?");
      }
    } else {
      Statement q(
          connection_.Handle(),
          "DELETE FROM SocialChatBlocks WHERE OwnerCharacterId = ? AND TargetCharacterId = ?");
      std::array params{Number(owner.character_id),
                        Number(target->character_id)};
      q.Execute(params);
    }
  } else if (action == CompanionSocialAction::kAdd) {
    if (Blocked(a, b)) {
      return {CompanionSocialStatus::kBlocked, target};
    }
    if (!requester) {
      if (RelationshipCount(a) >= 200 || RelationshipCount(b) >= 200) {
        return {CompanionSocialStatus::kCapacity, target};
      }
      Statement q(
          connection_.Handle(),
          "INSERT INTO SocialFriendships (FirstCharacterId, SecondCharacterId, RequestedBy) VALUES (?, ?, ?)");
      std::array params{Number(a), Number(b), Number(owner.character_id)};
      q.Execute(params);
    }
    // Reciprocal requests do not silently accept: only the recipient may
    // accept.
  } else if (action == CompanionSocialAction::kAccept) {
    if (Blocked(a, b)) {
      return {CompanionSocialStatus::kBlocked, target};
    }
    if (!requester || (!accepted && requester == owner.character_id)) {
      return {CompanionSocialStatus::kNoRequest, target};
    }
    pair_write(
        "UPDATE SocialFriendships SET IsAccepted = 1 WHERE FirstCharacterId = ? AND SecondCharacterId = ?");
  } else if (action == CompanionSocialAction::kDecline) {
    if (!requester || accepted || requester == owner.character_id) {
      return {CompanionSocialStatus::kNoRequest, target};
    }
    pair_write(
        "DELETE FROM SocialFriendships WHERE FirstCharacterId = ? AND SecondCharacterId = ?");
  } else if (action == CompanionSocialAction::kRemove) {
    pair_write(
        "DELETE FROM SocialFriendships WHERE FirstCharacterId = ? AND SecondCharacterId = ?");
  } else {
    return {CompanionSocialStatus::kInvalid, target};
  }
  tx.Commit();
  return {CompanionSocialStatus::kOk, target};
}
CompanionSocialStatus MySqlSocialStore::CanMessage(const CharacterRecord& owner,
                                                   std::uint32_t target) {
  if (!target || target == owner.character_id) {
    return CompanionSocialStatus::kSelf;
  }
  Transaction tx(connection_.Handle());
  LockPair(owner, target);
  if (Blocked(owner.character_id, target)) {
    return CompanionSocialStatus::kBlocked;
  }
  const auto a =
      (std::min)(owner.character_id, static_cast<std::uint64_t>(target));
  const auto b =
      (std::max)(owner.character_id, static_cast<std::uint64_t>(target));
  bool valid = false;
  {
    Statement q(
        connection_.Handle(),
        "SELECT IsAccepted FROM SocialFriendships WHERE FirstCharacterId = ? AND SecondCharacterId = ?");
    std::array params{Number(a), Number(b)};
    q.Execute(params);
    std::uint8_t accepted = 0;
    std::array result{Number(accepted)};
    valid = q.Fetch(result) && accepted == 1;
  }
  tx.Commit();
  return valid ? CompanionSocialStatus::kOk : CompanionSocialStatus::kNotFriend;
}
}  // namespace bubble_fighter::server::persistence
