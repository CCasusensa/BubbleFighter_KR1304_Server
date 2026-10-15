#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_MUTATION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_MUTATION_H_

#include <functional>
#include <variant>

#include "features/clans/clan_authentication.h"
#include "protocol/clans/clan_membership_messages.h"
#include "protocol/clans/clan_roster_messages.h"

namespace bubble_fighter::server::features::clans {
using ClanMutation =
    std::variant<protocol::ClanJoinRequest, protocol::ClanAutoJoinRequest,
                 protocol::ClanCancelJoinRequest, protocol::ClanLeaveRequest,
                 protocol::ClanMemberChangeRequest,
                 protocol::ClanRecruitmentRequest>;
struct ClanMutationResult {
  std::vector<protocol::ClanPacket> responses;
  std::vector<ClanDelivery> deliveries;
};
using ClanMutationAuthority = std::function<bool()>;
using ClanMutationHandler = std::function<ClanMutationResult(
    const ClanIdentity&, const ClanMutation&, const ClanMutationAuthority&)>;
}  // namespace bubble_fighter::server::features::clans

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_MUTATION_H_
