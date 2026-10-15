#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_DISBAND_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_DISBAND_H_

#include "features/clans/clan_mutation.h"
#include "protocol/clans/clan_disband_messages.h"

namespace bubble_fighter::server::features::clans {
using ClanDisbandHandler = std::function<ClanMutationResult(
    const ClanIdentity&, const protocol::ClanDisbandRequest&,
    const ClanMutationAuthority&)>;
}  // namespace bubble_fighter::server::features::clans

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_DISBAND_H_
