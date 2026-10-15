#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_PLAZA_ACCESS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_PLAZA_ACCESS_H_

#include "features/characters/character_store.h"
#include "features/clans/clan_channel_access.h"

namespace bubble_fighter::server::features::clans {
struct ClanPlazaScope {
  std::uint32_t clan_id = 0;
  std::u16string name;
  bool operator==(const ClanPlazaScope&) const = default;
};
struct ClanPlazaMember {
  ClanChannelMember member;
  std::u16string clan_name;
};
using ClanPlazaSource =
    std::function<std::optional<std::vector<ClanPlazaMember>>(
        std::span<const ClanIdentity>)>;
using ClanPlazaCommit = std::function<bool(const ClanPlazaScope&)>;
using ClanPlazaAuthority = std::function<bool(
    std::uint32_t, std::span<const characters::CharacterRecord>,
    const ClanPlazaCommit&)>;
struct ClanPlazaDecision {
  bool allowed = false;
  std::uint64_t revision = 0;
};
class ClanPlazaAccess {
 public:
  ClanPlazaAccess(std::shared_ptr<ClanAuthentication> authentication,
                  ClanPlazaSource source);
  std::uint64_t Revision() const;
  // The source completes one consistent DB read before invoking commit.
  // Lock order: coordinator/party -> clan operations -> completed DB -> scene.
  // Commit cannot reenter party/clan operations or perform database I/O.
  ClanPlazaDecision Authorize(std::uint32_t target, std::uint32_t category,
                              std::uint32_t minimum_level,
                              std::uint32_t maximum_level,
                              std::span<const ClanIdentity> identities,
                              const ClanPlazaCommit& commit = {}) const;

 private:
  std::shared_ptr<ClanAuthentication> authentication_;
  ClanPlazaSource source_;
};
}  // namespace bubble_fighter::server::features::clans

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_PLAZA_ACCESS_H_
