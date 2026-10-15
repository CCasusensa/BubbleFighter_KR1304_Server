#ifndef BUBBLEFIGHTER_SERVER_FEATURES_PARTIES_PARTY_SCENE_RESERVATION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_PARTIES_PARTY_SCENE_RESERVATION_H_

#include <functional>
#include <memory>
#include <vector>

#include "features/characters/character_store.h"

namespace bubble_fighter::server::features::parties {
class PartyRegistry;
struct PartySceneMember {
  std::uint64_t token = 0;
  characters::CharacterRecord character;
};

// A server-only authority; no token or reservation id is client supplied.
// Keep this alive until every connection adopts its outcome. Callbacks may
// enter a scene registry, but must not enter parties or invitation services.
class PartySceneReservation {
 public:
  PartySceneReservation(PartySceneReservation&& other) noexcept;
  PartySceneReservation& operator=(PartySceneReservation&& other) noexcept;
  PartySceneReservation(const PartySceneReservation&) = delete;
  PartySceneReservation& operator=(const PartySceneReservation&) = delete;
  ~PartySceneReservation();
  const std::vector<PartySceneMember>& Members() const noexcept;
  bool Commit(const std::function<bool()>& commit_scene);
  void Release() noexcept;

 private:
  friend class PartyRegistry;
  PartySceneReservation(std::weak_ptr<PartyRegistry> registry,
                        std::uint64_t party, std::uint64_t epoch,
                        std::uint64_t reservation,
                        std::vector<PartySceneMember> members) noexcept;
  std::weak_ptr<PartyRegistry> registry_;
  std::uint64_t party_ = 0, epoch_ = 0, reservation_ = 0;
  bool committed_ = false;
  std::vector<PartySceneMember> members_;
};
}  // namespace bubble_fighter::server::features::parties

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_PARTIES_PARTY_SCENE_RESERVATION_H_
