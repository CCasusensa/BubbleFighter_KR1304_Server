#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_AUTHENTICATION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_AUTHENTICATION_H_

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "features/authentication/credential_crypto.h"
#include "protocol/clans/clan_messages.h"

namespace bubble_fighter::server::features::clans {
struct ClanIdentity {
  std::uint64_t account_id = 0;
  std::uint32_t character_id = 0;
  bool operator==(const ClanIdentity&) const = default;
};

struct ClanDelivery {
  // Sorted, unique trusted character IDs and fully encoded packets prepared
  // before database commit. Offline recipients recover through profile reads.
  std::vector<std::uint32_t> characters;
  std::shared_ptr<const std::vector<protocol::ClanPacket>> packets;
};

struct ClanGameScene {
  std::uint32_t category = 60;
  // Native model types: 0 unknown, 1 menu, 2 room/practice (including waiting),
  // 3 plaza, 4 guild plaza, 6 clan plaza. Does not assert gameplay has started.
  std::uint32_t type = 0;
  bool operator==(const ClanGameScene&) const = default;
};

class ClanAuthentication {
  struct State;
  struct Game;
  struct Inbox;

 public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;
  using SecretFactory = std::function<authentication::CredentialDigest()>;
  using GameLink = std::shared_ptr<Game>;
  class Binding {
   public:
    Binding() = default;
    // Copies observe a connection without prolonging its lifetime. Only the
    // binding returned by Consume owns the inbox; moves transfer that owner.
    Binding(const Binding& other);
    Binding& operator=(const Binding& other);
    Binding(Binding&&) noexcept = default;
    Binding& operator=(Binding&&) noexcept = default;
    std::optional<ClanIdentity> Identity() const;
    std::optional<std::uint64_t> PlazaEpoch() const;
    std::optional<ClanGameScene> Scene() const;
    bool InClanChannel() const;
    bool SameConnection(const Binding& other) const;
    std::optional<std::vector<protocol::ClanPacket>> TakeNotifications();

   private:
    friend class ClanAuthentication;
    std::shared_ptr<State> state_;
    std::weak_ptr<Game> game_;
    std::uint64_t generation_ = 0;
    std::weak_ptr<Inbox> inbox_;
    std::shared_ptr<Inbox> owner_;
  };
  explicit ClanAuthentication(
      Now now = Clock::now,
      SecretFactory secrets = authentication::CredentialCrypto::GenerateSecret);
  // Only trusted game-login code may bind this identity. Resetting its last
  // GameLink revokes both outstanding credentials and every clan binding.
  GameLink BindGame(ClanIdentity identity);
  protocol::ClanCredentials Issue(const GameLink& game,
                                  std::u16string_view character_name);
  Binding Consume(const protocol::ClanCredentials& credentials);
  Binding Find(std::uint32_t character_id) const;
  // Called only by the owning main session after adopting or invalidating a
  // scene. Client C1045/C2025/C948 never call this API.
  void UpdateScene(const GameLink& game, ClanGameScene scene);
  // One atomic observation for the authorized roster. Auxiliary disconnects
  // do not hide a live main session; expired main links yield unknown (0).
  std::vector<std::uint32_t> ReadScenes(
      std::span<const std::uint32_t> characters) const;
  // Serializes clan connection operations across reads, commits and inbox
  // publication. Main game traffic and credential issuance use a separate lock.
  std::unique_lock<std::mutex> LockOperations();
  void Publish(std::span<const ClanDelivery> deliveries);
  // Cache invalidation only. Call after a trusted business handler returns,
  // while holding operations. Readers still load authoritative membership.
  void InvalidateMembership();
  std::uint64_t MembershipRevision() const;
  // Trusted final channel commit, with operations held. Independent of each
  // player's later scene adoption; never called from client state reports.
  void PublishChannelCategory(std::span<const ClanIdentity> identities,
                              std::uint32_t category);

 private:
  std::shared_ptr<State> state_;
};
}  // namespace bubble_fighter::server::features::clans

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_AUTHENTICATION_H_
