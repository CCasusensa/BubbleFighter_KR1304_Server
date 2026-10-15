#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_DIRECTORY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_DIRECTORY_H_

#include <functional>
#include <optional>

#include "features/clans/clan_authentication.h"
#include "protocol/clans/clan_directory_messages.h"

namespace bubble_fighter::server::features::clans {
// Five rows follows the native UI. Query bounds and exact-name matching are
// project policy; original server matching/sorting limits remain unknown.
inline bool IsClanDirectoryQuerySupported(
    const protocol::ClanDirectoryRequest& request) {
  if (!request.page || request.page > 10000) return false;
  if (request.query_mode == 4)
    return !request.search_text.empty() && request.search_text.size() <= 12 &&
           request.search_text.find(u'\0') == std::u16string::npos;
  return request.query_mode >= 5 && request.query_mode <= 8 &&
         request.search_text.empty();
}
// Null means the authenticated owner is unavailable, not an empty directory.
using ClanDirectorySource =
    std::function<std::optional<protocol::ClanDirectoryResponse>(
        const ClanIdentity&, const protocol::ClanDirectoryRequest&)>;
}  // namespace bubble_fighter::server::features::clans

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_DIRECTORY_H_
