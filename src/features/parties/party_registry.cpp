#include "features/parties/party_registry.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "features/parties/solo_party.h"

namespace bubble_fighter::server::features::parties {
PartyMembership::PartyMembership(std::shared_ptr<PartyRegistry> registry,
                                 std::uint64_t token)
    : registry_(std::move(registry)), token_(token) {}
PartyMembership::PartyMembership(PartyMembership&& other) noexcept
    : registry_(std::move(other.registry_)),
      token_(std::exchange(other.token_, 0)) {}
PartyMembership& PartyMembership::operator=(PartyMembership&& other) noexcept {
  if (this != &other) {
    Release();
    registry_ = std::move(other.registry_);
    token_ = std::exchange(other.token_, 0);
  }
  return *this;
}
PartyMembership::~PartyMembership() { Release(); }
PartyMembership::operator bool() const noexcept { return registry_ != nullptr; }
void PartyMembership::Release() noexcept {
  if (registry_) registry_->Release(token_);
  registry_.reset();
  token_ = 0;
}
protocol::UpdatePartyNotification PartyMembership::Snapshot() const {
  if (!registry_) throw std::logic_error("Missing party membership.");
  return registry_->Snapshot(token_);
}
void PartyMembership::Update(
    const characters::CharacterRecord& character, bool busy,
    std::optional<protocol::PartySupplementaryRecord> equipment,
    std::optional<std::uint32_t> clan_id) {
  if (!registry_) throw std::logic_error("Missing party membership.");
  registry_->Update(token_, character, busy, std::move(equipment), clan_id);
}
characters::CharacterRecord PartyMembership::Character() const {
  if (!registry_) throw std::logic_error("Missing party membership.");
  return registry_->Character(token_);
}
PartyJoinStatus PartyMembership::JoinApproved(
    const PartyTarget& target, const std::function<bool()>& commit_scene,
    const PartyGroupAuthority& authority) {
  if (!registry_) throw std::logic_error("Missing party membership.");
  return registry_->JoinApproved(token_, target, commit_scene, authority);
}
bool PartyMembership::CheckAccess(const PartyAccessCheck& check) {
  if (!registry_) throw std::logic_error("Missing party membership.");
  return registry_->CheckAccess(token_, check);
}
bool PartyMembership::Leave() {
  if (!registry_) throw std::logic_error("Missing party membership.");
  return registry_->Leave(token_);
}
void PartyMembership::Kick(const std::u16string& target_name) {
  if (!registry_) throw std::logic_error("Missing party membership.");
  registry_->Kick(token_, target_name);
}
void PartyMembership::ChangeLeader(const std::u16string& target_name) {
  if (!registry_) throw std::logic_error("Missing party membership.");
  registry_->ChangeLeader(token_, target_name);
}

PartyRegistry::PartyRegistry(std::size_t capacity, AllowsPair allows_pair)
    : capacity_(capacity), allows_pair_(std::move(allows_pair)) {
  if (capacity < 2 || capacity > 8)
    throw std::invalid_argument("Invalid party capacity.");
}
std::uint64_t PartyRegistry::NextId() {
  if (next_id_ == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("Party identity space exhausted.");
  return next_id_++;
}
PartyMembership PartyRegistry::Enter(
    const characters::CharacterRecord& character, PartyPacketSink sink,
    std::uint32_t clan_id) {
  SoloParty::Create(character);  // Same trusted identity checks as login.
  if (!sink) throw std::invalid_argument("Missing party packet sink.");
  std::lock_guard lock(mutex_);
  for (const auto& [token, member] : members_) {
    if (member.character.account_id == character.account_id ||
        member.character.character_id == character.character_id ||
        member.character.character_name == character.character_name)
      throw std::invalid_argument("Duplicate party identity.");
  }
  auto self = shared_from_this();
  const auto token = NextId();
  const auto party_id = NextId();
  parties_.emplace(party_id, Party{NextId(), {token}});
  try {
    members_.emplace(
        token,
        Member{character, std::move(sink), party_id, false, {}, clan_id});
  } catch (...) {
    parties_.erase(party_id);
    throw;
  }
  return PartyMembership(std::move(self), token);
}
protocol::UpdatePartyNotification PartyRegistry::Roster(
    const Party& party) const {
  protocol::UpdatePartyNotification result;
  result.state = 2;  // E041 ordinary party state; no invented movement enum.
  for (const auto token : party.members) {
    const auto& member = members_.at(token);
    result.members.push_back(
        SoloParty::Create(member.character).members.front());
    // Native1141670 maps this final DWORD to member+44;90AB30 compares clans.
    result.members.back().values_at32[3] = member.clan_id;
    if (member.equipment)
      result.supplementary_records.push_back(*member.equipment);
  }
  return result;
}
protocol::UpdatePartyNotification PartyRegistry::Snapshot(
    std::uint64_t token) const {
  std::lock_guard lock(mutex_);
  return Roster(parties_.at(members_.at(token).party_id));
}
characters::CharacterRecord PartyRegistry::Character(
    std::uint64_t token) const {
  std::lock_guard lock(mutex_);
  return members_.at(token).character;
}
std::optional<PartyTarget> PartyRegistry::Capture(
    const std::u16string& name) const {
  std::lock_guard lock(mutex_);
  for (const auto& [token, member] : members_) {
    if (member.character.character_name == name) {
      return PartyTarget{member.party_id, parties_.at(member.party_id).epoch,
                         token, name};
    }
  }
  return {};
}
bool PartyRegistry::Busy(const Party& party) const {
  return party.scene_reservation != 0 ||
         std::any_of(party.members.begin(), party.members.end(),
                     [&](auto token) {
                       return members_.at(token).busy ||
                              members_.at(token).access_revoked;
                     });
}
bool PartyRegistry::Owns(const PartyMembership& membership) const noexcept {
  return membership.registry_.get() == this;
}
void PartyRegistry::PlanRoster(
    std::vector<Publication>& plan, const Party& party,
    const std::optional<protocol::TcpPacket>& notice) const {
  if (party.members.empty()) return;
  const auto roster = Roster(party).Encode();
  for (const auto token : party.members) {
    Publication publication{token, {roster}};
    if (notice) publication.packets.push_back(*notice);
    plan.push_back(std::move(publication));
  }
}
void PartyRegistry::PlanDeparture(std::vector<Publication>& plan,
                                  const Party& before, const Party& after,
                                  std::uint64_t departing) const {
  if (after.members.empty()) return;
  const auto& name = members_.at(departing).character.character_name;
  const auto notice =
      before.members.front() == departing
          ? protocol::PartyLeaderLeftNotification{name,
                                                  members_
                                                      .at(after.members.front())
                                                      .character.character_name}
                .Encode()
          : protocol::PartyMemberLeftNotification{name}.Encode();
  PlanRoster(plan, after, notice);
}
void PartyRegistry::Publish(std::vector<Publication> plan) noexcept {
  for (auto& publication : plan) {
    const auto found = members_.find(publication.recipient);
    if (found == members_.end()) continue;
    try {
      found->second.sink(std::move(publication.packets));
    } catch (...) {
      // A disconnected recipient cannot interrupt delivery to its former party.
    }
  }
}
void PartyRegistry::Update(
    std::uint64_t token, const characters::CharacterRecord& character,
    bool busy, std::optional<protocol::PartySupplementaryRecord> equipment,
    std::optional<std::uint32_t> clan_id) {
  SoloParty::Create(character);
  if (equipment && equipment->key != character.character_id)
    throw std::invalid_argument("Party equipment identity mismatch.");
  std::lock_guard lock(mutex_);
  auto& member = members_.at(token);
  if (character.account_id != member.character.account_id ||
      character.character_id != member.character.character_id)
    throw std::invalid_argument("Party member identity changed.");
  for (const auto& [other, candidate] : members_) {
    if (other != token &&
        candidate.character.character_name == character.character_name)
      throw std::invalid_argument("Duplicate party identity.");
  }
  const bool renamed =
      character.character_name != member.character.character_name;
  const auto next_clan = clan_id.value_or(member.clan_id);
  const bool clan_changed = next_clan != member.clan_id;
  const bool changed = renamed || clan_changed || equipment != member.equipment;
  auto& party = parties_.at(member.party_id);
  Member next{character,
              member.sink,
              member.party_id,
              busy,
              std::move(equipment),
              next_clan,
              member.access_revoked};
  if (!changed) {
    if (busy != member.busy) party.epoch = NextId();
    member = std::move(next);
    return;
  }
  // Encode a candidate before commit so malformed supplementary data cannot
  // partially mutate the stored member or escape through a later broadcast.
  auto candidate = Roster(party);
  for (auto& item : candidate.members) {
    if (item.identity_value == character.character_id) {
      item.character_name = character.character_name;
      item.values_at32[3] = next_clan;
    }
  }
  std::erase_if(candidate.supplementary_records, [&](const auto& item) {
    return item.key == character.character_id;
  });
  if (next.equipment)
    candidate.supplementary_records.push_back(*next.equipment);
  const auto packet = candidate.Encode();
  std::vector<Publication> plan;
  if (changed) {
    for (const auto recipient : party.members)
      plan.push_back({recipient, {packet}});
  }
  if (renamed || clan_changed || busy != member.busy) party.epoch = NextId();
  member = std::move(next);
  Publish(std::move(plan));
}
PartyJoinStatus PartyRegistry::JoinApproved(
    std::uint64_t token, const PartyTarget& target,
    const std::function<bool()>& commit_scene,
    const PartyGroupAuthority& authority) {
  std::lock_guard lock(mutex_);
  const auto owner = members_.find(target.member_token);
  const auto found = parties_.find(target.party_id);
  if (owner == members_.end() || found == parties_.end() ||
      owner->second.party_id != target.party_id ||
      found->second.epoch != target.epoch ||
      owner->second.character.character_name != target.member_name)
    return PartyJoinStatus::kStale;
  auto& visitor = members_.at(token);
  if (visitor.party_id == target.party_id)
    return PartyJoinStatus::kAlreadyJoined;
  auto& source = parties_.at(visitor.party_id);
  auto& destination = found->second;
  if (Busy(source) || Busy(destination)) return PartyJoinStatus::kBusy;
  if (destination.members.size() >= capacity_) return PartyJoinStatus::kFull;
  if (allows_pair_) {
    for (const auto peer : destination.members)
      if (!allows_pair_(visitor.character.character_id,
                        members_.at(peer).character.character_id))
        return PartyJoinStatus::kBlocked;
  }
  auto old_party = source, new_party = destination;
  std::erase(old_party.members, token);
  old_party.epoch = NextId();
  new_party.members.push_back(token);
  // Existing consent from this destination remains usable while capacity lasts.
  std::vector<Publication> plan;
  PlanDeparture(plan, source, old_party, token);
  PlanRoster(
      plan, new_party,
      protocol::PartyMemberJoinedNotification{visitor.character.character_name}
          .Encode());
  // No registry allocation, packet encoding or callback copy follows scene
  // commit. A failed scene transition cannot leave a joined party behind.
  static_assert(std::is_nothrow_move_assignable_v<Party>);
  auto commit = [&] {
    if (commit_scene && !commit_scene()) return false;
    const auto old_id = visitor.party_id;
    source = std::move(old_party);
    destination = std::move(new_party);
    visitor.party_id = target.party_id;
    if (source.members.empty()) parties_.erase(old_id);
    Publish(std::move(plan));
    return true;
  };
  std::vector<characters::CharacterRecord> identities;
  if (authority)
    for (const auto member : new_party.members)
      identities.push_back(members_.at(member).character);
  return (authority ? authority(identities, commit) : commit())
             ? PartyJoinStatus::kJoined
             : PartyJoinStatus::kStale;
}
bool PartyRegistry::CheckAccess(std::uint64_t token,
                                const PartyAccessCheck& check) {
  std::lock_guard lock(mutex_);
  const auto& member = members_.at(token);
  if (member.access_revoked) return false;
  const auto& party = parties_.at(member.party_id);
  std::vector<characters::CharacterRecord> identities;
  for (const auto peer : party.members)
    identities.push_back(members_.at(peer).character);
  bool allowed = false;
  try {
    allowed = check && check(member.party_id, party.epoch, identities);
  } catch (...) {
    for (const auto peer : party.members)
      members_.at(peer).access_revoked = true;
    throw;
  }
  if (!allowed)
    for (const auto peer : party.members)
      members_.at(peer).access_revoked = true;
  return allowed;
}
bool PartyRegistry::Leave(std::uint64_t token) {
  std::lock_guard lock(mutex_);
  auto& member = members_.at(token);
  auto& party = parties_.at(member.party_id);
  if (party.members.size() == 1 || Busy(party)) return false;
  auto remaining = party;
  std::erase(remaining.members, token);
  remaining.epoch = NextId();
  Party solo{NextId(), {token}};
  std::vector<Publication> plan;
  PlanDeparture(plan, party, remaining, token);
  PlanRoster(plan, solo, protocol::PartyLeaveNotification{}.Encode());
  const auto new_id = NextId();
  parties_.emplace(new_id, std::move(solo));
  party = std::move(remaining);
  member.party_id = new_id;
  Publish(std::move(plan));
  return true;
}
void PartyRegistry::Release(std::uint64_t token) noexcept {
  std::lock_guard lock(mutex_);
  const auto found = members_.find(token);
  if (found == members_.end()) return;
  const auto party_id = found->second.party_id;
  auto& party = parties_.at(party_id);
  std::vector<Publication> plan;
  try {
    auto remaining = party;
    std::erase(remaining.members, token);
    PlanDeparture(plan, party, remaining, token);
  } catch (...) {
    plan.clear();
  }
  std::erase(party.members, token);
  // Token and party IDs are never reused. Even if the counter is exhausted,
  // remaining members must not retain a disconnected identity.
  if (next_id_ != std::numeric_limits<std::uint64_t>::max())
    party.epoch = next_id_++;
  members_.erase(found);
  if (party.members.empty()) parties_.erase(party_id);
  Publish(std::move(plan));
}
void PartyRegistry::Kick(std::uint64_t token, const std::u16string& name) {
  std::lock_guard lock(mutex_);
  const auto& leader = members_.at(token);
  auto& party = parties_.at(leader.party_id);
  const auto target =
      std::find_if(members_.begin(), members_.end(), [&](const auto& item) {
        return item.second.character.character_name == name;
      });
  const auto reason = target == members_.end()                        ? 1U
                      : target->first == token                        ? 2U
                      : target->second.party_id != leader.party_id    ? 3U
                      : party.members.front() != token || Busy(party) ? 4U
                                                                      : 0U;
  std::vector<Publication> plan{
      {token, {protocol::KickPartyResponse{reason, name}.Encode()}}};
  if (reason != 0) {
    Publish(std::move(plan));
    return;
  }
  auto remaining = party;
  std::erase(remaining.members, target->first);
  remaining.epoch = NextId();
  Party solo{NextId(), {target->first}};
  const auto roster = Roster(remaining).Encode();
  plan[0].packets.insert(plan[0].packets.begin(), roster);
  for (const auto peer : remaining.members) {
    if (peer != token)
      plan.push_back({peer,
                      {roster,
                       protocol::PartyKickNotification{
                           leader.character.character_name, name}
                           .Encode()}});
  }
  PlanRoster(
      plan, solo,
      protocol::KickedFromPartyNotification{leader.character.character_name}
          .Encode());
  const auto new_id = NextId();
  parties_.emplace(new_id, std::move(solo));
  party = std::move(remaining);
  target->second.party_id = new_id;
  Publish(std::move(plan));
}
void PartyRegistry::ChangeLeader(std::uint64_t token,
                                 const std::u16string& name) {
  std::lock_guard lock(mutex_);
  const auto& member = members_.at(token);
  auto& party = parties_.at(member.party_id);
  const auto target =
      std::find_if(party.members.begin(), party.members.end(), [&](auto peer) {
        return members_.at(peer).character.character_name == name;
      });
  const auto reason = target == party.members.end()      ? 1U
                      : *target == party.members.front() ? 2U
                      : party.members.front() != token   ? 3U
                      : Busy(party)                      ? 4U
                                                         : 0U;
  const auto response =
      protocol::ChangePartyLeaderResponse{
          name, members_.at(party.members.front()).character.character_name,
          reason}
          .Encode();
  if (reason != 0) {
    Publish({{token, {response}}});
    return;
  }
  auto changed = party;
  std::rotate(changed.members.begin(),
              changed.members.begin() + (target - party.members.begin()),
              changed.members.begin() + (target - party.members.begin()) + 1);
  changed.epoch = NextId();
  std::vector<Publication> plan;
  PlanRoster(plan, changed, response);
  party = std::move(changed);
  Publish(std::move(plan));
}
}  // namespace bubble_fighter::server::features::parties
