#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_PROFILE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_PROFILE_H_

#include <functional>
#include <optional>

#include "features/clans/clan_authentication.h"

namespace bubble_fighter::server::features::clans {
struct ClanSnapshot {
  protocol::ClanProfileResponse profile;
  std::optional<protocol::ClanDetailsResponse> details;
  // Durable maintenance removal; canonical membership remains absent.
  std::optional<protocol::ClanProfileResponse> disband_notice;
  std::optional<protocol::ClanProfileResponse> training_exit_notice;
  std::uint64_t training_exit_generation = 0;
};
// Null means the trusted account/character is unavailable, not "no clan".
using ClanProfileSource =
    std::function<std::optional<ClanSnapshot>(const ClanIdentity&)>;
}  // namespace bubble_fighter::server::features::clans

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_PROFILE_H_
