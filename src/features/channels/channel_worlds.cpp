#include "features/channels/channel_worlds.h"

#include <limits>
#include <stdexcept>

namespace bubble_fighter::server::features::channels {
bool ChannelWorld::Allows(
    const characters::CharacterRecord& character) const noexcept {
  return character.account_id && character.character_id &&
         character.statistics.IsValid() &&
         character.statistics.level >= levels.minimum &&
         character.statistics.level <= levels.maximum;
}
ChannelWorlds::ChannelWorlds(
    const ChannelRules& rules, std::shared_ptr<parties::PartyRegistry> parties,
    social::RoomInvitations::AllowsPair allows_pair,
    rooms::RoomMatchSettler settler, rooms::MatchRewardPolicy rewards,
    std::shared_ptr<clans::ClanChannelAccess> clan_access,
    std::shared_ptr<clans::ClanPlazaAccess> clan_plaza_access) {
  if (!parties) throw std::invalid_argument("Missing channel party registry.");
  if (rules.Categories().contains(80) && !clan_access)
    throw std::invalid_argument(
        "Clan channel requires authenticated clan service.");
  party_transfers_ = std::make_shared<PartyChannelTransfers>(parties);
  const auto settlements =
      settler ? std::make_shared<rooms::RoomSettlementQueue>(std::move(settler))
              : nullptr;
  for (const auto& [category, levels] : rules.Categories()) {
    auto world = std::make_shared<ChannelWorld>();
    world->category = category;
    world->levels = levels;
    world->clan_access = clan_access;
    world->clan_plaza_access = clan_plaza_access;
    world->maximum_mercenaries = rules.MaximumMercenaries();
    world->rooms =
        rooms::RoomRegistry::Create(512, {}, {}, {}, rewards, settlements);
    world->plazas = std::make_shared<lobby::PlazaRegistry>();
    clans::ClanPlazaAuthority plaza_authority =
        [weak = std::weak_ptr<const ChannelWorld>(world)](
            auto clan_id, auto members, const auto& commit) {
          const auto owner = weak.lock();
          return owner &&
                 owner->AuthorizePlaza(clan_id, members, commit).allowed;
        };
    world->invitations = std::make_shared<social::RoomInvitations>(
        world->rooms, allows_pair, parties, world->plazas, plaza_authority);
    world->transfers = std::make_shared<parties::PartySceneTransfers>(
        parties, world->rooms, world->plazas, plaza_authority);
    worlds_.emplace(category, std::move(world));
  }
}
clans::ClanChannelDecision ChannelWorld::Authorize(
    std::span<const characters::CharacterRecord> members,
    const std::function<bool()>& commit, bool entering) const {
  if (members.empty() || members.size() > 8) return {};
  std::vector<clans::ClanIdentity> identities;
  identities.reserve(members.size());
  for (const auto& member : members) {
    if (!member.account_id || !member.character_id ||
        (category != 80 && !Allows(member)) ||
        member.character_id > std::numeric_limits<std::uint32_t>::max())
      return {};
    identities.push_back(
        {member.account_id, static_cast<std::uint32_t>(member.character_id)});
  }
  if (clan_access)
    return clan_access->Authorize(category, levels.minimum, levels.maximum,
                                  maximum_mercenaries, identities, commit,
                                  entering);
  if (category == 80 || (commit && !commit())) return {};
  return {0, 0};
}
std::shared_ptr<const ChannelWorld> ChannelWorlds::Find(
    std::uint32_t category) const {
  const auto found = worlds_.find(category);
  return found == worlds_.end() ? nullptr : found->second;
}
clans::ClanPlazaDecision ChannelWorld::AuthorizePlaza(
    std::uint32_t clan_id, std::span<const characters::CharacterRecord> members,
    const clans::ClanPlazaCommit& commit) const {
  if (!clan_plaza_access || members.empty() || members.size() > 8) return {};
  std::vector<clans::ClanIdentity> identities;
  identities.reserve(members.size());
  for (const auto& member : members) {
    if (!member.account_id || !member.character_id ||
        member.character_id > std::numeric_limits<std::uint32_t>::max())
      return {};
    identities.push_back(
        {member.account_id, static_cast<std::uint32_t>(member.character_id)});
  }
  return clan_plaza_access->Authorize(clan_id, category, levels.minimum,
                                      levels.maximum, identities, commit);
}
std::shared_ptr<PartyChannelTransfers> ChannelWorlds::PartyTransfers()
    const noexcept {
  return party_transfers_;
}
}  // namespace bubble_fighter::server::features::channels
