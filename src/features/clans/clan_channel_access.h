#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_CHANNEL_ACCESS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_CHANNEL_ACCESS_H_

#include "features/clans/clan_authentication.h"

namespace bubble_fighter::server::features::clans {
struct ClanChannelMember {
  ClanIdentity identity;
  std::uint32_t level = 0;
  std::uint32_t clan_id = 0;
  std::uint32_t membership_status = 0;
  bool enabled = false;
};
// One consistent read, in request order. The source must finish its database
// transaction before returning; the callback below may acquire scene locks.
using ClanChannelSource =
    std::function<std::optional<std::vector<ClanChannelMember>>(
        std::span<const ClanIdentity>)>;
struct ClanChannelDecision {
  std::uint32_t result = 2;
  std::uint64_t revision = 0;
};
class ClanChannelAccess {
 public:
  ClanChannelAccess(std::shared_ptr<ClanAuthentication> authentication,
                    ClanChannelSource source);
  std::uint64_t Revision() const;
  // Lock order: party (if any), clan operations, database read (ends), scene.
  // Commit must not reenter parties/clan operations or perform database I/O.
  // Only actual channel commits set entering=true, never preflight/residency.
  ClanChannelDecision Authorize(std::uint32_t category,
                                std::uint32_t minimum_level,
                                std::uint32_t maximum_level,
                                std::uint32_t maximum_mercenaries,
                                std::span<const ClanIdentity> identities,
                                const std::function<bool()>& commit = {},
                                bool entering = false) const;

 private:
  std::shared_ptr<ClanAuthentication> authentication_;
  ClanChannelSource source_;
};
}  // namespace bubble_fighter::server::features::clans

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_CHANNEL_ACCESS_H_
