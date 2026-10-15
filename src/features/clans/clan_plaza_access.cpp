#include "features/clans/clan_plaza_access.h"

#include <set>
#include <stdexcept>
#include <utility>

#include "features/characters/character_creation.h"

namespace bubble_fighter::server::features::clans {
ClanPlazaAccess::ClanPlazaAccess(
    std::shared_ptr<ClanAuthentication> authentication, ClanPlazaSource source)
    : authentication_(std::move(authentication)), source_(std::move(source)) {
  if (!authentication_ || !source_)
    throw std::invalid_argument("Missing clan plaza authority.");
}
std::uint64_t ClanPlazaAccess::Revision() const {
  return authentication_->MembershipRevision();
}
ClanPlazaDecision ClanPlazaAccess::Authorize(
    std::uint32_t target, std::uint32_t category, std::uint32_t minimum_level,
    std::uint32_t maximum_level, std::span<const ClanIdentity> identities,
    const ClanPlazaCommit& commit) const {
  if (!target || target > 0x7FFFFFFFU ||
      (category != 20 && category != 60 && category != 70 && category != 80) ||
      (category == 80 && target == 99) || minimum_level < 1 ||
      maximum_level > 260 || minimum_level > maximum_level ||
      identities.empty() || identities.size() > 8)
    return {};
  std::set<std::uint64_t> accounts;
  std::set<std::uint32_t> characters;
  for (const auto& identity : identities)
    if (!identity.account_id || !identity.character_id ||
        !accounts.insert(identity.account_id).second ||
        !characters.insert(identity.character_id).second)
      return {};
  auto operation = authentication_->LockOperations();
  ClanPlazaDecision decision{false, authentication_->MembershipRevision()};
  const auto rows = source_(identities);
  if (!rows || rows->size() != identities.size()) return decision;
  const auto& name = rows->front().clan_name;
  if (name.empty() || name.size() > 12 ||
      characters::CharacterCreation::ValidateName(name))
    return decision;
  for (std::size_t i = 0; i < identities.size(); ++i) {
    const auto& row = rows->at(i);
    const auto& member = row.member;
    if (member.identity != identities[i] || !member.enabled ||
        member.level < minimum_level || member.level > maximum_level ||
        member.clan_id != target || member.membership_status < 100 ||
        member.membership_status > 102 ||
        (target == 99 && member.membership_status != 100) ||
        row.clan_name != name)
      return decision;
  }
  if (commit && !commit({target, name})) return decision;
  decision.allowed = true;
  return decision;
}
}  // namespace bubble_fighter::server::features::clans
