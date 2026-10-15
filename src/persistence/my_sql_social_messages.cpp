#include <algorithm>
#include <array>
#include <stdexcept>

#include "features/chat/scene_chat.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_social_store.h"
#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
using namespace features::social;
using namespace protocol;
namespace {
void MessageTime(std::uint64_t now) {
  if (!now || now > CompanionMailboxNotification::kMaximumTimestamp) {
    throw std::invalid_argument("Invalid mailbox clock.");
  }
}
std::u16string MessageText(std::span<const std::uint8_t> bytes) {
  if (bytes.size() % 2) {
    throw std::runtime_error("Invalid stored mailbox text.");
  }
  PacketReader reader(bytes);
  std::u16string result;
  while (reader.RemainingBytes()) {
    result.push_back(reader.ReadUInt16());
  }
  if (!features::chat::SceneChat::ValidText(result)) {
    throw std::runtime_error("Invalid stored mailbox text.");
  }
  return result;
}
}  // namespace

void MySqlSocialStore::ExpireMessages(std::uint64_t recipient,
                                      std::uint64_t now) {
  const auto cutoff =
      now > kMessageLifetimeSeconds ? now - kMessageLifetimeSeconds : 0;
  Statement query(
      connection_.Handle(),
      "DELETE FROM SocialMessages WHERE RecipientCharacterId = ? AND SentAtUnixSeconds <= ?");
  std::array parameters{Number(recipient), Number(cutoff)};
  query.Execute(parameters);
}

SocialMessageChange MySqlSocialStore::QueueMessage(
    const features::characters::CharacterRecord& owner,
    const CompanionSocialRequest& request, std::uint64_t now) {
  MessageTime(now);
  (void)request.Encode();
  if (request.action != CompanionSocialAction::kMessage ||
      !features::chat::SceneChat::ValidText(request.text)) {
    return {CompanionSocialStatus::kInvalid, {}, {}, 0};
  }
  auto target = Find(request.target_id, request.target_name);
  if (!target) {
    return {CompanionSocialStatus::kNotFound, {}, {}, 0};
  }
  if (target->character_id == owner.character_id) {
    return {CompanionSocialStatus::kSelf, target, {}, 0};
  }
  const auto first =
      (std::min)(owner.character_id, std::uint64_t{target->character_id});
  const auto second =
      (std::max)(owner.character_id, std::uint64_t{target->character_id});
  Transaction tx(connection_.Handle());
  LockPair(owner, target->character_id);
  // The same ordered locks are used by friendship, block and name changes.
  // Resolve canonical names again after acquiring them.
  target = Find(target->character_id, request.target_name);
  if (!target) {
    return {CompanionSocialStatus::kNotFound, {}, {}, 0};
  }
  const auto snapshot = LoadLocked(owner);
  if (Blocked(first, second)) {
    return {CompanionSocialStatus::kBlocked, target, {}, 0};
  }
  {
    Statement query(
        connection_.Handle(),
        "SELECT IsAccepted FROM SocialFriendships WHERE FirstCharacterId = ? AND SecondCharacterId = ?");
    std::array parameters{Number(first), Number(second)};
    query.Execute(parameters);
    std::uint8_t accepted = 0;
    std::array result{Number(accepted)};
    if (!query.Fetch(result) || accepted != 1) {
      return {CompanionSocialStatus::kNotFriend, target, {}, 0};
    }
  }
  ExpireMessages(target->character_id, now);
  {
    Statement query(
        connection_.Handle(),
        "SELECT COUNT(*) FROM SocialMessages WHERE RecipientCharacterId = ?");
    std::array parameters{Number(target->character_id)};
    query.Execute(parameters);
    std::uint64_t count = 0;
    std::array result{Number(count)};
    if (!query.Fetch(result)) {
      throw std::runtime_error("Missing mailbox capacity.");
    }
    if (count >= kMailboxCapacity) {
      return {CompanionSocialStatus::kCapacity, target, {}, 0};
    }
  }
  PacketWriter writer;
  for (auto unit : request.text) writer.WriteUInt16(unit);
  const auto bytes = writer.TakeBytes();
  {
    Statement query(
        connection_.Handle(),
        "INSERT INTO SocialMessages (SenderCharacterId, RecipientCharacterId, FirstCharacterId, SecondCharacterId, SentAtUnixSeconds, TextUtf16) VALUES (?, ?, ?, ?, ?, ?)");
    std::array parameters{
        Number(owner.character_id),
        Number(target->character_id),
        Number(first),
        Number(second),
        Number(now),
        Bytes(bytes.data(), static_cast<unsigned long>(bytes.size()))};
    query.Execute(parameters);
  }
  const auto message_id = mysql_insert_id(connection_.Handle());
  // Both recipient and sender replies must be representable before committing.
  (void)CompanionMailboxNotification{
      CompanionMailboxStatus::kOk,
      {{message_id, static_cast<std::uint32_t>(owner.character_id), now,
        snapshot.owner.character_name, request.text}}}
      .Encode();
  (void)CompanionPrivateMessageNotification{target->character_id, target->name,
                                            snapshot.owner.character_name,
                                            request.text}
      .Encode();
  tx.Commit();
  return {CompanionSocialStatus::kOk, target, snapshot.owner.character_name,
          message_id};
}

SocialMailboxSnapshot MySqlSocialStore::LoadMailbox(
    const features::characters::CharacterRecord& owner, std::uint64_t now) {
  MessageTime(now);
  if (!owner.character_id || owner.character_id > UINT32_MAX) {
    throw std::invalid_argument("Invalid mailbox owner.");
  }
  Transaction tx(connection_.Handle());
  LockPair(owner, static_cast<std::uint32_t>(owner.character_id));
  SocialMailboxSnapshot mailbox{
      CompanionMailboxStatus::kOk, LoadLocked(owner), {}};
  ExpireMessages(owner.character_id, now);
  {
    // All relationship mutations involving the recipient share its character
    // lock. Name/account state is read from this transaction's current
    // snapshot.
    Statement query(
        connection_.Handle(),
        "SELECT m.MessageId, m.SenderCharacterId, m.SentAtUnixSeconds, m.TextUtf16 FROM SocialMessages m JOIN SocialFriendships f ON f.FirstCharacterId = m.FirstCharacterId AND f.SecondCharacterId = m.SecondCharacterId JOIN Characters c ON c.CharacterId = m.SenderCharacterId JOIN Accounts a ON a.AccountId = c.AccountId WHERE m.RecipientCharacterId = ? AND m.SentAtUnixSeconds <= ? AND f.IsAccepted = 1 AND a.IsDisabled = 0 AND NOT EXISTS (SELECT 1 FROM SocialChatBlocks b WHERE (b.OwnerCharacterId = m.SenderCharacterId AND b.TargetCharacterId = m.RecipientCharacterId) OR (b.OwnerCharacterId = m.RecipientCharacterId AND b.TargetCharacterId = m.SenderCharacterId)) ORDER BY m.MessageId LIMIT 20");
    std::array parameters{Number(owner.character_id), Number(now)};
    query.Execute(parameters);
    std::uint64_t id = 0, sender = 0, timestamp = 0;
    std::array<std::uint8_t, 512> raw{};
    unsigned long length = 0;
    std::array result{
        Number(id), Number(sender), Number(timestamp),
        Bytes(raw.data(), static_cast<unsigned long>(raw.size()))};
    result[3].length = &length;
    while (query.Fetch(result)) {
      const auto peer = std::find_if(
          mailbox.snapshot.rows.begin(), mailbox.snapshot.rows.end(),
          [&](const auto& row) {
            return row.character_id == sender && row.relationship == 1;
          });
      if (peer == mailbox.snapshot.rows.end() || sender > UINT32_MAX) {
        throw std::runtime_error("Mailbox snapshot lacks its friendship.");
      }
      mailbox.messages.push_back({id, static_cast<std::uint32_t>(sender),
                                  timestamp, peer->name,
                                  MessageText(std::span(raw).first(length))});
    }
  }
  (void)CompanionMailboxNotification{mailbox.status, mailbox.messages}.Encode();
  tx.Commit();
  return mailbox;
}

void MySqlSocialStore::AcknowledgeMessage(
    const features::characters::CharacterRecord& owner,
    std::uint64_t message_id) {
  if (!message_id || !owner.character_id || owner.character_id > UINT32_MAX) {
    throw std::invalid_argument("Invalid mailbox acknowledgement.");
  }
  Transaction tx(connection_.Handle());
  LockPair(owner, static_cast<std::uint32_t>(owner.character_id));
  Statement query(
      connection_.Handle(),
      "DELETE FROM SocialMessages WHERE RecipientCharacterId = ? AND MessageId = ?");
  std::array parameters{Number(owner.character_id), Number(message_id)};
  query.Execute(parameters);
  tx.Commit();
}
}  // namespace bubble_fighter::server::persistence
