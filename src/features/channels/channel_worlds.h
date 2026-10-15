#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHANNELS_CHANNEL_WORLDS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHANNELS_CHANNEL_WORLDS_H_

#include <filesystem>
#include <istream>
#include <map>
#include <memory>
#include <string>

#include "features/channels/party_channel_transfers.h"
#include "features/clans/clan_channel_access.h"
#include "features/parties/party_scene_transfers.h"
#include "features/social/room_invitations.h"

namespace bubble_fighter::server::features::channels {
struct ChannelLevelRange {
  std::uint32_t minimum = 1;
  std::uint32_t maximum = 260;
};
// Category names are native; enablement and level ranges are project policy.
class ChannelRules {
 public:
  static ChannelRules Read(std::istream& input);
  static ChannelRules Load(const std::filesystem::path& path);
  const std::map<std::uint32_t, ChannelLevelRange>& Categories() const noexcept;
  std::uint32_t MaximumMercenaries() const noexcept;

 private:
  std::map<std::uint32_t, ChannelLevelRange> categories_{{60, {1, 260}}};
  std::uint32_t maximum_mercenaries_ = 1;
};
struct ChannelWorld {
  std::uint32_t category = 60;
  ChannelLevelRange levels;
  std::shared_ptr<rooms::RoomRegistry> rooms;
  std::shared_ptr<lobby::PlazaRegistry> plazas;
  std::shared_ptr<social::RoomInvitations> invitations;
  std::shared_ptr<parties::PartySceneTransfers> transfers;
  std::shared_ptr<clans::ClanChannelAccess> clan_access;
  std::shared_ptr<clans::ClanPlazaAccess> clan_plaza_access;
  std::uint32_t maximum_mercenaries = 1;
  bool Allows(const characters::CharacterRecord& character) const noexcept;
  clans::ClanChannelDecision Authorize(
      std::span<const characters::CharacterRecord> members,
      const std::function<bool()>& commit = {}, bool entering = false) const;
  clans::ClanPlazaDecision AuthorizePlaza(
      std::uint32_t clan_id,
      std::span<const characters::CharacterRecord> members,
      const clans::ClanPlazaCommit& commit = {}) const;
};
// Immutable directory after construction. Social identity and parties remain
// global; room IDs, plaza IDs, invitation authority and scene queues are
// scoped.
class ChannelWorlds {
 public:
  ChannelWorlds(const ChannelRules& rules,
                std::shared_ptr<parties::PartyRegistry> parties,
                social::RoomInvitations::AllowsPair allows_pair = {},
                rooms::RoomMatchSettler settler = {},
                rooms::MatchRewardPolicy rewards = {},
                std::shared_ptr<clans::ClanChannelAccess> clan_access = {},
                std::shared_ptr<clans::ClanPlazaAccess> clan_plaza_access = {});
  std::shared_ptr<const ChannelWorld> Find(std::uint32_t category) const;
  std::shared_ptr<PartyChannelTransfers> PartyTransfers() const noexcept;

 private:
  std::map<std::uint32_t, std::shared_ptr<const ChannelWorld>> worlds_;
  std::shared_ptr<PartyChannelTransfers> party_transfers_;
};
}  // namespace bubble_fighter::server::features::channels
#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHANNELS_CHANNEL_WORLDS_H_
