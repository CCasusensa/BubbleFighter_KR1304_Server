#include "features/parties/party_scene_reservation.h"

#include <utility>

#include "features/parties/party_registry.h"

namespace bubble_fighter::server::features::parties {
PartySceneReservation::PartySceneReservation(
    std::weak_ptr<PartyRegistry> registry, std::uint64_t party,
    std::uint64_t epoch, std::uint64_t reservation,
    std::vector<PartySceneMember> members) noexcept
    : registry_(std::move(registry)),
      party_(party),
      epoch_(epoch),
      reservation_(reservation),
      members_(std::move(members)) {}
PartySceneReservation::PartySceneReservation(
    PartySceneReservation&& other) noexcept
    : registry_(std::move(other.registry_)),
      party_(other.party_),
      epoch_(other.epoch_),
      reservation_(std::exchange(other.reservation_, 0)),
      committed_(other.committed_),
      members_(std::move(other.members_)) {}
PartySceneReservation& PartySceneReservation::operator=(
    PartySceneReservation&& other) noexcept {
  if (this != &other) {
    Release();
    registry_ = std::move(other.registry_);
    party_ = other.party_;
    epoch_ = other.epoch_;
    reservation_ = std::exchange(other.reservation_, 0);
    committed_ = other.committed_;
    members_ = std::move(other.members_);
  }
  return *this;
}
PartySceneReservation::~PartySceneReservation() { Release(); }
const std::vector<PartySceneMember>& PartySceneReservation::Members()
    const noexcept {
  return members_;
}
void PartySceneReservation::Release() noexcept {
  if (reservation_ != 0) {
    if (auto registry = registry_.lock()) {
      std::lock_guard lock(registry->mutex_);
      const auto found = registry->parties_.find(party_);
      if (found != registry->parties_.end() &&
          found->second.scene_reservation == reservation_)
        found->second.scene_reservation = 0;
    }
    reservation_ = 0;
  }
}
bool PartySceneReservation::Commit(const std::function<bool()>& commit_scene) {
  const auto registry = registry_.lock();
  if (!registry || reservation_ == 0 || committed_ || !commit_scene)
    return false;
  std::lock_guard lock(registry->mutex_);
  const auto found = registry->parties_.find(party_);
  if (found == registry->parties_.end()) return false;
  const auto& party = found->second;
  if (party.epoch != epoch_ || party.scene_reservation != reservation_ ||
      party.members.size() != members_.size())
    return false;
  for (std::size_t i = 0; i < members_.size(); ++i) {
    if (party.members[i] != members_[i].token ||
        registry->members_.at(members_[i].token).busy ||
        registry->members_.at(members_[i].token).access_revoked)
      return false;
  }
  // No allocation or exception-capable work may follow successful publication.
  if (!commit_scene()) return false;
  committed_ = true;
  return true;
}
std::optional<PartySceneReservation> PartyRegistry::ReserveScene(
    const PartyMembership& leader, bool allow_solo) {
  if (!Owns(leader)) return {};
  std::lock_guard lock(mutex_);
  const auto member = members_.find(leader.token_);
  if (member == members_.end()) return {};
  auto& party = parties_.at(member->second.party_id);
  if ((!allow_solo && party.members.size() < 2) || party.members.empty() ||
      party.members.front() != leader.token_ || Busy(party))
    return {};
  std::vector<PartySceneMember> participants;
  for (const auto token : party.members)
    participants.push_back({token, members_.at(token).character});
  const auto id = NextId();
  PartySceneReservation result{weak_from_this(), member->second.party_id,
                               party.epoch, id, std::move(participants)};
  party.scene_reservation = id;
  return result;
}
}  // namespace bubble_fighter::server::features::parties
