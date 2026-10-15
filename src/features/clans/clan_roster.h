#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_ROSTER_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_ROSTER_H_

#include <functional>
#include <optional>
#include <vector>

#include "features/clans/clan_authentication.h"
#include "protocol/clans/clan_roster_messages.h"

namespace bubble_fighter::server::features::clans {
enum class ClanRosterKind { kMembers, kDetails, kApplicants };
// The source resolves membership and permission from this trusted identity.
// A missing owner returns null; a stale membership returns a profile refresh.
// Applicants require fresh details before the list because the native consumer
// uses cached recruitment state to decide whether to send rejection requests.
using ClanRosterSource =
    std::function<std::optional<std::vector<protocol::ClanPacket>>(
        const ClanIdentity&, ClanRosterKind)>;
}  // namespace bubble_fighter::server::features::clans

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_ROSTER_H_
