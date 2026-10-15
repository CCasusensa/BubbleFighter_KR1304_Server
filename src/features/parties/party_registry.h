#ifndef BUBBLEFIGHTER_SERVER_FEATURES_PARTIES_PARTY_REGISTRY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_PARTIES_PARTY_REGISTRY_H_

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "features/characters/character_store.h"
#include "features/parties/party_scene_reservation.h"
#include "protocol/parties/party_messages.h"
#include "protocol/scenes/update_party_notification.h"

namespace bubble_fighter::server::features::channels {
class PartyChannelTransfers;
}
namespace bubble_fighter::server::features::parties {
using PartyPacketSink = std::function<void(std::vector<protocol::TcpPacket>)>;
using PartyGroupAuthority =
    std::function<bool(std::span<const characters::CharacterRecord>,
                       const std::function<bool()>&)>;
using PartyAccessCheck =
    std::function<bool(std::uint64_t, std::uint64_t,
                       std::span<const characters::CharacterRecord>)>;
class PartyRegistry;
class PartySceneTransfers;

// Internal admission target captured when consent is requested. It is never
// decoded from a client packet, and it does not replace invitation consent.
struct PartyTarget {
  std::uint64_t party_id = 0;
  std::uint64_t epoch = 0;
  std::uint64_t member_token = 0;
  std::u16string member_name;
};
enum class PartyJoinStatus {
  kJoined,
  kStale,
  kAlreadyJoined,
  kFull,
  kBusy,
  kBlocked
};

class PartyMembership {
 public:
  PartyMembership() = default;
  PartyMembership(PartyMembership&& other) noexcept;
  PartyMembership& operator=(PartyMembership&& other) noexcept;
  PartyMembership(const PartyMembership&) = delete;
  PartyMembership& operator=(const PartyMembership&) = delete;
  ~PartyMembership();
  explicit operator bool() const noexcept;
  void Release() noexcept;
  protocol::UpdatePartyNotification Snapshot() const;
  characters::CharacterRecord Character() const;
  void Update(const characters::CharacterRecord& character, bool busy,
              std::optional<protocol::PartySupplementaryRecord> equipment = {},
              std::optional<std::uint32_t> clan_id = {});
  // The caller must have independently verified an invitation or visitation
  // approval for this connection and target before invoking this method.
  // Optional scene commit executes after all party validation/encoding and
  // before its no-throw commit. False/throw must leave the scene unchanged.
  // The callback may lock a scene registry, but must not reenter parties.
  PartyJoinStatus JoinApproved(const PartyTarget& target,
                               const std::function<bool()>& commit_scene = {},
                               const PartyGroupAuthority& authority = {});
  // Runs under the party lock. Failure permanently revokes this complete
  // membership set, including peers that poll after another peer disconnects.
  bool CheckAccess(const PartyAccessCheck& check);
  bool Leave();
  void Kick(const std::u16string& target_name);
  void ChangeLeader(const std::u16string& target_name);

 private:
  friend class PartyRegistry;
  friend class PartySceneTransfers;
  friend class channels::PartyChannelTransfers;
  PartyMembership(std::shared_ptr<PartyRegistry> registry, std::uint64_t token);
  std::shared_ptr<PartyRegistry> registry_;
  std::uint64_t token_ = 0;
};

// Connection-owned parties, including single-member parties. All membership
// edits and publications are serialized. Sinks must only enqueue; they must not
// call this registry. Pair checks may consult the cached social block table.
// Capacity is explicit project policy, not a recovered original server limit.
class PartyRegistry : public std::enable_shared_from_this<PartyRegistry> {
 public:
  using AllowsPair = std::function<bool(std::uint64_t, std::uint64_t)>;
  explicit PartyRegistry(std::size_t capacity, AllowsPair allows_pair = {});
  PartyMembership Enter(const characters::CharacterRecord& character,
                        PartyPacketSink sink, std::uint32_t clan_id = 0);
  std::optional<PartyTarget> Capture(const std::u16string& member_name) const;
  bool Owns(const PartyMembership& membership) const noexcept;
  // Leader-only whole-party reservation. It freezes joins/administration;
  // disconnects, name changes and busy changes invalidate its commit.
  std::optional<PartySceneReservation> ReserveScene(
      const PartyMembership& leader, bool allow_solo = false);

 private:
  friend class PartyMembership;
  friend class PartySceneReservation;
  struct Member {
    characters::CharacterRecord character;
    PartyPacketSink sink;
    std::uint64_t party_id = 0;
    bool busy = false;
    std::optional<protocol::PartySupplementaryRecord> equipment;
    // Trusted display snapshot only; never authorizes clan/channel admission.
    std::uint32_t clan_id = 0;
    bool access_revoked = false;
  };
  struct Party {
    std::uint64_t epoch = 0;
    std::vector<std::uint64_t> members;  // First member is the leader.
    std::uint64_t scene_reservation = 0;
  };
  struct Publication {
    std::uint64_t recipient = 0;
    std::vector<protocol::TcpPacket> packets;
  };
  protocol::UpdatePartyNotification Snapshot(std::uint64_t token) const;
  characters::CharacterRecord Character(std::uint64_t token) const;
  protocol::UpdatePartyNotification Roster(const Party& party) const;
  void Update(std::uint64_t token, const characters::CharacterRecord& character,
              bool busy,
              std::optional<protocol::PartySupplementaryRecord> equipment,
              std::optional<std::uint32_t> clan_id);
  PartyJoinStatus JoinApproved(std::uint64_t token, const PartyTarget& target,
                               const std::function<bool()>& commit_scene,
                               const PartyGroupAuthority& authority);
  bool CheckAccess(std::uint64_t token, const PartyAccessCheck& check);
  bool Leave(std::uint64_t token);
  void Release(std::uint64_t token) noexcept;
  void Kick(std::uint64_t token, const std::u16string& target_name);
  void ChangeLeader(std::uint64_t token, const std::u16string& target_name);
  bool Busy(const Party& party) const;
  std::uint64_t NextId();
  void PlanRoster(std::vector<Publication>& plan, const Party& party,
                  const std::optional<protocol::TcpPacket>& notice = {}) const;
  void PlanDeparture(std::vector<Publication>& plan, const Party& before,
                     const Party& after, std::uint64_t departing) const;
  void Publish(std::vector<Publication> plan) noexcept;
  std::size_t capacity_;
  AllowsPair allows_pair_;
  mutable std::mutex mutex_;
  std::uint64_t next_id_ = 1;
  std::map<std::uint64_t, Member> members_;
  std::map<std::uint64_t, Party> parties_;
};
}  // namespace bubble_fighter::server::features::parties

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_PARTIES_PARTY_REGISTRY_H_
