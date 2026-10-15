#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_CREATION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_CREATION_H_

#include <functional>

#include "features/clans/clan_authentication.h"
#include "features/inventory/inventory_slot.h"
#include "protocol/inventory/item_use_request.h"

namespace bubble_fighter::server::features::clans {
using ClanCreationAuthority = std::function<bool()>;
using ClanCreateHandler = std::function<protocol::ClanCreateResponse(
    const ClanIdentity&, const protocol::ClanCreateRequest&, std::uint64_t,
    const ClanCreationAuthority&)>;
using ClanConfirmationHandler = std::function<std::vector<protocol::TcpPacket>(
    const ClanIdentity&, const protocol::ItemUseRequest&, std::uint64_t)>;

class ClanCreation {
 public:
  // Only the currently verified Count / Function 22 / Parameter 30 card.
  static bool SupportsItem(std::uint32_t item_id);
  // Returns the native creation result: 1 valid, -100/-101 length, 0 other.
  // Character restrictions are project policy; no invented banned-word list.
  static std::uint32_t ValidateRequest(
      const protocol::ClanCreateRequest& request);
  // Private-copy preparation; no changes on refusal. Database ownership,
  // membership and uniqueness must be checked by the transactional caller.
  static bool ConsumeCard(inventory::CharacterInventory& inventory,
                          std::uint32_t level, std::uint32_t item_id,
                          std::uint64_t observed_at_unix_seconds);
};
}  // namespace bubble_fighter::server::features::clans

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_CREATION_H_
