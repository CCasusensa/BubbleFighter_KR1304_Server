#include "features/clans/clan_channel_access.h"

#include <set>
#include <stdexcept>
#include <utility>

namespace bubble_fighter::server::features::clans {
ClanChannelAccess::ClanChannelAccess(
    std::shared_ptr<ClanAuthentication> authentication,
    ClanChannelSource source)
    : authentication_(std::move(authentication)), source_(std::move(source)) {
  if (!authentication_ || !source_)
    throw std::invalid_argument("Missing clan channel authority.");
}
std::uint64_t ClanChannelAccess::Revision() const {
  return authentication_->MembershipRevision();
}
ClanChannelDecision ClanChannelAccess::Authorize(
    std::uint32_t category, std::uint32_t minimum_level,
    std::uint32_t maximum_level, std::uint32_t maximum_mercenaries,
    std::span<const ClanIdentity> identities,
    const std::function<bool()>& commit, bool entering) const {
  if ((category != 20 && category != 60 && category != 70 && category != 80) ||
      minimum_level < 1 || maximum_level > 260 ||
      minimum_level > maximum_level || maximum_mercenaries > 3 ||
      identities.empty() || identities.size() > 8 || (entering && !commit))
    return {};
  std::set<std::uint64_t> accounts;
  std::set<std::uint32_t> characters;
  for (const auto& identity : identities) {
    if (!identity.account_id || !identity.character_id ||
        !accounts.insert(identity.account_id).second ||
        !characters.insert(identity.character_id).second)
      return {};
  }
  auto operation = authentication_->LockOperations();
  ClanChannelDecision decision{2, authentication_->MembershipRevision()};
  if (category == 80) {
    const auto members = source_(identities);
    if (!members || members->size() != identities.size()) return decision;
    const auto accepted = [](const ClanChannelMember& member) {
      return member.clan_id && member.membership_status >= 100 &&
             member.membership_status <= 102;
    };
    for (std::size_t i = 0; i < members->size(); ++i) {
      const auto& member = members->at(i);
      if (member.identity != identities[i] || !member.enabled ||
          member.level < minimum_level || member.level > maximum_level)
        return decision;
    }
    const auto& leader = members->front();
    if (!accepted(leader) || leader.clan_id == 99) {
      decision.result = 3;
      return decision;
    }
    std::uint32_t outsiders = 0;
    for (const auto& member : *members)
      if (!accepted(member) || member.clan_id != leader.clan_id) ++outsiders;
    if (outsiders > maximum_mercenaries) {
      decision.result = 4;
      return decision;
    }
  }
  if (commit && !commit()) return decision;
  if (entering) authentication_->PublishChannelCategory(identities, category);
  decision.result = 0;
  return decision;
}
}  // namespace bubble_fighter::server::features::clans
