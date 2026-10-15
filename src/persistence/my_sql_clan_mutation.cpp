#include <algorithm>
#include <array>
#include <map>
#include <type_traits>

#include "features/characters/character_creation.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"
#include "protocol/codec/packet_reader.h"

namespace bubble_fighter::server::persistence {
using namespace my_sql_commands;
using namespace protocol;
using namespace features::clans;
namespace {
constexpr std::uint32_t kReject = 0xfffffff6U;
constexpr std::uint32_t kLeave = 0xffffffecU;
constexpr std::uint32_t kKick = 0xffffffe2U;
struct Member {
  std::uint32_t clan = 0;
  std::uint32_t role = 0;
};
struct Character {
  std::uint64_t account = 0;
  std::u16string name;
  bool disabled = false;
};
std::u16string Text(std::span<const std::uint8_t> bytes) {
  if (bytes.size() % 2)
    throw std::runtime_error("Invalid clan mutation state.");
  PacketReader reader(bytes);
  std::u16string result;
  while (reader.RemainingBytes()) {
    const auto unit = reader.ReadUInt16();
    if (!unit) throw std::runtime_error("Invalid clan mutation state.");
    result.push_back(unit);
  }
  return result;
}
Member Membership(MYSQL* connection, std::uint32_t character) {
  Statement query(
      connection,
      "SELECT ClanId, MembershipStatus FROM ClanMembers WHERE CharacterId = ?");
  std::array params{Number(character)};
  query.Execute(params);
  Member result;
  std::array fields{Number(result.clan), Number(result.role)};
  query.Fetch(fields);
  return result;
}
void Deliver(ClanMutationResult& result, std::vector<std::uint32_t> targets,
             std::vector<ClanPacket> packets) {
  if (targets.empty()) return;
  for (const auto& packet : packets) ClanPacketCodec::Encode(packet);
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  result.deliveries.push_back(
      {std::move(targets),
       std::make_shared<const std::vector<ClanPacket>>(std::move(packets))});
}
}  // namespace

ClanMutationResult MySqlClanStore::Mutate(
    const ClanIdentity& identity, const ClanMutation& request,
    const ClanMutationAuthority& authorized) {
  if (!authorized || !authorized())
    throw std::runtime_error("Clan mutation authorization expired.");
  std::visit([](const auto& value) { value.Encode(); }, request);
  if (!identity.account_id || !identity.character_id)
    throw std::runtime_error("Clan mutation owner is unavailable.");
  auto* connection = connection_.Handle();
  {
    // Read committed after the per-clan lock observes earlier committed
    // mutations even if the owner lookup waited. Every mutation locks rows in
    // character-ID order, then the clan; names and target membership stay
    // bound.
    Statement isolation(connection,
                        "SET TRANSACTION ISOLATION LEVEL READ COMMITTED");
    isolation.Execute({});
  }
  Transaction transaction(connection);
  const auto change = std::get_if<ClanMemberChangeRequest>(&request);
  std::vector<std::uint32_t> lock_ids{identity.character_id};
  if (change && change->character_id &&
      change->character_id != identity.character_id)
    lock_ids.push_back(change->character_id);
  std::sort(lock_ids.begin(), lock_ids.end());
  std::map<std::uint32_t, Character> characters;
  for (const auto id : lock_ids) {
    Statement query(
        connection,
        "SELECT c.AccountId, c.NameUtf16, a.IsDisabled FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId WHERE c.CharacterId = ? FOR UPDATE");
    std::array params{Number(id)};
    query.Execute(params);
    Character row;
    std::array<std::uint8_t, 32> bytes{};
    unsigned long length = 0;
    std::uint8_t disabled = 0;
    std::array fields{
        Number(row.account),
        Bytes(bytes.data(), static_cast<unsigned long>(bytes.size())),
        Number(disabled)};
    fields[1].length = &length;
    if (!query.Fetch(fields)) continue;
    if (!length || length > bytes.size())
      throw std::runtime_error("Invalid clan mutation state.");
    row.name = Text(std::span(bytes).first(length));
    if (features::characters::CharacterCreation::ValidateName(row.name))
      throw std::runtime_error("Invalid clan mutation state.");
    row.disabled = disabled != 0;
    characters.emplace(id, std::move(row));
  }
  const auto actor = characters.find(identity.character_id);
  if (actor == characters.end() ||
      actor->second.account != identity.account_id || actor->second.disabled)
    throw std::runtime_error("Clan mutation owner is unavailable.");
  const auto current = Membership(connection, identity.character_id);
  const auto join = std::get_if<ClanJoinRequest>(&request);
  const auto automatic = std::get_if<ClanAutoJoinRequest>(&request);
  const auto cancel = std::get_if<ClanCancelJoinRequest>(&request);
  const auto recruitment = std::get_if<ClanRecruitmentRequest>(&request);
  const bool leave = std::holds_alternative<ClanLeaveRequest>(request);
  std::uint32_t clan_id = join        ? join->clan_id
                          : automatic ? automatic->clan_id
                                      : current.clan;
  std::uint32_t mode = 0, target_role = 0;
  std::u16string target_name;
  const auto profile = [&] {
    auto snapshot = LoadInTransaction(identity);
    if (!snapshot)
      throw std::runtime_error("Clan mutation owner is unavailable.");
    return *snapshot;
  };
  const auto refuse = [&](std::uint32_t code = 0) {
    ClanMutationResult result;
    if (join || automatic) {
      const auto trusted = profile().profile;
      result.responses.push_back(ClanJoinResponse{
          code, trusted.membership_status, trusted.clan_id, trusted.clan_name}
                                     .Encode());
    } else if (change) {
      result.responses.push_back(
          ClanMemberChangeResponse{code, target_name, target_role, target_role}
              .Encode());
    } else if (recruitment) {
      result.responses.push_back(
          ClanRecruitmentResponse{0, code, mode}.Encode());
    } else if (cancel) {
      result.responses.push_back(ClanCancelJoinResponse{code, 0}.Encode());
    } else {
      result.responses.push_back(profile().profile.Encode());
    }
    return result;  // Transaction destructor releases locks without writes.
  };
  if ((join || automatic) && current.role) return refuse(0xfffffffeU);
  if (!clan_id || clan_id == 99) return refuse();
  std::u16string clan_name;
  std::uint32_t capacity = 0;
  std::uint64_t disband = 0;
  {
    Statement query(
        connection,
        "SELECT NameUtf16, MemberCapacity, IsRecruiting, DisbandAtUnixSeconds FROM Clans WHERE ClanId = ? FOR UPDATE");
    std::array params{Number(clan_id)};
    query.Execute(params);
    std::array<std::uint8_t, 24> bytes{};
    unsigned long length = 0;
    std::array fields{
        Bytes(bytes.data(), static_cast<unsigned long>(bytes.size())),
        Number(capacity), Number(mode), Number(disband)};
    fields[0].length = &length;
    if (!query.Fetch(fields)) return refuse();
    if (length < 8 || length > bytes.size() || !capacity || capacity > 65535 ||
        mode > 2)
      throw std::runtime_error("Invalid clan mutation state.");
    clan_name = Text(std::span(bytes).first(length));
  }
  std::vector<std::uint32_t> accepted, officers;
  std::size_t applicants = 0, masters = 0, deputies = 0;
  {
    Statement query(
        connection,
        "SELECT CharacterId, MembershipStatus FROM ClanMembers WHERE ClanId = ? ORDER BY CharacterId LIMIT 66037");
    std::array params{Number(clan_id)};
    query.Execute(params);
    std::uint64_t id = 0;
    std::uint32_t role = 0;
    std::array fields{Number(id), Number(role)};
    std::size_t count = 0;
    while (query.Fetch(fields)) {
      if (++count > 66036 || !id || id > 0xffffffffULL ||
          (role != 1 && role != 100 && role != 101 && role != 102))
        throw std::runtime_error("Invalid clan mutation state.");
      if (role == 1) {
        ++applicants;
        continue;
      }
      accepted.push_back(static_cast<std::uint32_t>(id));
      if (role == 101 || role == 102)
        officers.push_back(static_cast<std::uint32_t>(id));
      masters += role == 101;
      deputies += role == 102;
    }
    if (masters != 1 || deputies > 1 || accepted.size() > capacity)
      throw std::runtime_error("Invalid clan mutation state.");
  }
  ClanMutationResult result;
  const auto write_member = [&](std::uint32_t id, std::uint32_t old_role,
                                std::uint32_t new_role) {
    if (!old_role) {
      Statement insert(
          connection,
          "INSERT INTO ClanMembers (CharacterId, ClanId, MembershipStatus) VALUES (?, ?, ?)");
      std::array params{Number(id), Number(clan_id), Number(new_role)};
      insert.Execute(params);
      if (insert.AffectedRows() != 1)
        throw std::runtime_error("Clan mutation write failed.");
      {
        Statement clear(connection,
                        "DELETE FROM ClanDisbandNotices WHERE CharacterId = ?");
        std::array clear_params{Number(id)};
        clear.Execute(clear_params);
        Statement history(
            connection,
            "UPDATE TrainingClanHistory SET ExitStatus = 0 WHERE CharacterId = ?");
        history.Execute(clear_params);
      }
    } else if (!new_role) {
      Statement erase(
          connection,
          "DELETE FROM ClanMembers WHERE CharacterId = ? AND ClanId = ? AND MembershipStatus = ?");
      std::array params{Number(id), Number(clan_id), Number(old_role)};
      erase.Execute(params);
      if (erase.AffectedRows() != 1)
        throw std::runtime_error("Clan mutation write failed.");
    } else {
      Statement update(
          connection,
          "UPDATE ClanMembers SET MembershipStatus = ? WHERE CharacterId = ? AND ClanId = ? AND MembershipStatus = ?");
      std::array params{Number(new_role), Number(id), Number(clan_id),
                        Number(old_role)};
      update.Execute(params);
      if (update.AffectedRows() != 1)
        throw std::runtime_error("Clan mutation write failed.");
    }
  };
  if (join || automatic) {
    if ((join ? join->clan_name : automatic->clan_name) != clan_name ||
        (automatic && automatic->character_name != actor->second.name))
      return refuse();
    if (disband) return refuse(0xfffffffdU);
    if (!mode) return refuse(0xfffffffbU);
    if (automatic && mode != 2) return refuse(0xfffffffaU);
    if (join && mode != 1) return refuse();
    if (join && applicants >= 500) return refuse(0xffffffffU);
    if (automatic && accepted.size() >= capacity) return refuse();
    const std::uint32_t role = automatic ? 100U : 1U;
    write_member(identity.character_id, 0, role);
    result.responses.push_back(
        ClanJoinResponse{1, role, clan_id, clan_name}.Encode());
    if (automatic) {
      const auto fresh = profile();
      result.responses.push_back(fresh.profile.Encode());
      auto roster =
          ReadRosterInTransaction(identity, ClanRosterKind::kMembers).value();
      roster.insert(roster.begin(), fresh.details->Encode());
      Deliver(result, accepted, std::move(roster));
    } else {
      Deliver(result, officers,
              {ClanApplicationChangedNotification{}.Encode()});
    }
  } else if (recruitment) {
    if (recruitment->clan_id != clan_id ||
        (current.role != 101 && current.role != 102) ||
        recruitment->recruitment_mode > 2 || disband)
      return refuse();
    const auto requested = recruitment->recruitment_mode;
    if (mode != requested) {
      Statement update(connection,
                       "UPDATE Clans SET IsRecruiting = ? WHERE ClanId = ?");
      std::array params{Number(requested), Number(clan_id)};
      update.Execute(params);
      if (update.AffectedRows() != 1)
        throw std::runtime_error("Clan mutation write failed.");
    }
    result.responses.push_back(
        ClanRecruitmentResponse{0, 1, requested}.Encode());
    std::erase(accepted, identity.character_id);
    std::erase(officers, identity.character_id);
    Deliver(result, accepted, {profile().details->Encode()});
    if (!requested && applicants)
      Deliver(result, officers,
              {ClanApplicationChangedNotification{}.Encode()});
  } else if (cancel) {
    if (cancel->clan_id != clan_id || current.role != 1) return refuse();
    write_member(identity.character_id, 1, 0);
    result.responses = {ClanCancelJoinResponse{1, 0}.Encode(),
                        profile().profile.Encode()};
    Deliver(result, officers,
            {ClanJoinCancelledNotification{1, 0, actor->second.name}.Encode()});
  } else if (leave) {
    if (current.role != 100 && current.role != 102) return refuse();
    write_member(identity.character_id, current.role, 0);
    const auto packet =
        ClanLeaveNotification{clan_name, identity.character_id,
                              actor->second.name, current.role, kLeave}
            .Encode();
    result.responses = {packet, profile().profile.Encode()};
    std::erase(accepted, identity.character_id);
    Deliver(result, accepted, {packet});
  } else if (change) {
    if ((current.role != 101 && current.role != 102) ||
        change->character_id == identity.character_id)
      return refuse();
    const auto target = characters.find(change->character_id);
    if (target == characters.end()) return refuse();
    const auto membership = Membership(connection, change->character_id);
    if (membership.clan != clan_id) return refuse();
    target_role = membership.role;
    target_name = target->second.name;
    if (change->old_status != target_role ||
        change->character_name != target_name)
      return refuse();
    const auto next = change->new_status;
    const bool approve = target_role == 1 && next == 100;
    const bool reject = target_role == 1 && next == kReject;
    const bool promote =
        current.role == 101 && target_role == 100 && next == 102;
    const bool demote =
        current.role == 101 && target_role == 102 && next == 100;
    const bool kick =
        next == kKick &&
        (target_role == 100 || (target_role == 102 && current.role == 101));
    if (!approve && !reject && !promote && !demote && !kick) return refuse();
    if ((approve || promote) && (disband || target->second.disabled))
      return refuse();
    if (approve && !mode) return refuse();
    if (approve && accepted.size() >= capacity) return refuse(2);
    if (promote && deputies) return refuse(0xffffff9aU);
    std::vector<ClanPacket> target_packets;
    const ClanIdentity target_identity{target->second.account,
                                       change->character_id};
    if (approve) {
      // Seed the *actual previous* pending state for native 51D330, which
      // reads manager +244 on 1->100. Final trusted profile follows the event.
      target_packets.push_back(
          LoadInTransaction(target_identity)->profile.Encode());
    }
    write_member(change->character_id, target_role, reject || kick ? 0U : next);
    result.responses.push_back(
        ClanMemberChangeResponse{1, target_name, next, target_role}.Encode());
    const auto notification =
        ClanMemberChangedNotification{clan_name, change->character_id,
                                      target_name, next, target_role}
            .Encode();
    target_packets.push_back(notification);
    // Disabled targets can be removed but have no authorized profile reader.
    if (!target->second.disabled)
      target_packets.push_back(
          LoadInTransaction(target_identity)->profile.Encode());
    Deliver(result, {change->character_id}, std::move(target_packets));
    if (!reject) {
      std::erase(accepted, change->character_id);
      Deliver(result, accepted, {notification});
    }
    if (approve || reject) {
      std::erase(officers, identity.character_id);
      Deliver(result, officers,
              {ClanApplicationChangedNotification{}.Encode()});
    }
  }
  for (const auto& packet : result.responses) ClanPacketCodec::Encode(packet);
  if (!authorized())
    throw std::runtime_error("Clan mutation authorization expired.");
  transaction.Commit();
  return result;
}
}  // namespace bubble_fighter::server::persistence
