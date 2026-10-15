#include "features/clans/clan_authentication.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "features/characters/character_creation.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::features::clans {
using authentication::CredentialCrypto;
using authentication::CredentialDigest;

struct ClanAuthentication::Inbox {
  std::array<std::shared_ptr<const std::vector<protocol::ClanPacket>>, 64>
      batches;
  std::size_t count = 0;
  bool failed = false;
};
struct ClanAuthentication::Game {
  std::weak_ptr<State> state;
  ClanIdentity identity;
  std::uint64_t generation = 0;
  std::optional<CredentialDigest> pending;
  std::weak_ptr<Inbox> inbox;
  bool plaza_available = false;
  std::uint64_t plaza_epoch = 0;
  ClanGameScene scene;
  std::uint32_t committed_category = 60;
};
struct ClanAuthentication::State {
  struct Grant {
    std::weak_ptr<Game> game;
    protocol::ClanCredentials metadata;  // Passport deliberately absent.
    Clock::time_point deadline;
  };
  std::mutex mutex;
  std::mutex operations;
  Now now;
  SecretFactory secrets;
  std::uint64_t serial = 0;
  std::uint64_t membership_revision = 0;
  std::map<CredentialDigest, Grant> grants;
  std::map<std::uint64_t, std::weak_ptr<Game>> games;

  void Cleanup() {
    const auto time = now();
    std::erase_if(grants, [&](const auto& row) {
      return row.second.game.expired() || time >= row.second.deadline;
    });
    std::erase_if(games, [](const auto& row) { return row.second.expired(); });
  }
};

ClanAuthentication::ClanAuthentication(Now now, SecretFactory secrets)
    : state_(std::make_shared<State>()) {
  if (!now || !secrets)
    throw std::invalid_argument("Missing clan clock or secret source.");
  state_->now = std::move(now);
  state_->secrets = std::move(secrets);
}

ClanAuthentication::GameLink ClanAuthentication::BindGame(
    ClanIdentity identity) {
  if (!identity.account_id || !identity.character_id)
    throw std::invalid_argument("Missing trusted clan identity.");
  std::lock_guard lock(state_->mutex);
  state_->Cleanup();
  // Project capacity, separate from native clan member limits. Duplicate
  // game logins must never steal an existing game's outstanding grant.
  if (state_->games.size() >= 128 ||
      state_->games.contains(identity.account_id))
    throw std::runtime_error("Clan game identity is already bound or full.");
  for (const auto& [account, weak] : state_->games) {
    if (const auto existing = weak.lock();
        existing && existing->identity.character_id == identity.character_id)
      throw std::runtime_error("Clan game identity is already bound or full.");
  }
  auto game = std::make_shared<Game>();
  game->state = state_;
  game->identity = identity;
  state_->games.emplace(identity.account_id, game);
  return game;
}

protocol::ClanCredentials ClanAuthentication::Issue(
    const GameLink& game, std::u16string_view character_name) {
  if (!game || game->state.lock() != state_ ||
      characters::CharacterCreation::ValidateName(character_name))
    throw std::invalid_argument("Invalid clan credential issuer.");
  std::lock_guard lock(state_->mutex);
  state_->Cleanup();
  if (state_->serial == std::numeric_limits<std::uint64_t>::max())
    throw std::runtime_error("Clan credential serial exhausted.");
  const auto secret = state_->secrets();
  protocol::PacketWriter writer;
  writer.WriteUInt64(++state_->serial);
  for (const auto byte : secret) writer.WriteUInt8(byte);
  protocol::ClanCredentials credentials{game->identity.character_id,
                                        std::u16string(character_name), 1, 0,
                                        writer.TakeBytes()};
  // Finish response construction before publishing its grant.
  protocol::ClanPassportResponse{credentials}.Encode();
  const auto digest = CredentialCrypto::Sha256(credentials.passport);
  protocol::ClanCredentials metadata{credentials.value_at0,
                                     credentials.text_at4,
                                     credentials.value_at28,
                                     credentials.value_at32,
                                     {}};
  State::Grant grant{game, std::move(metadata),
                     state_->now() + std::chrono::seconds{30}};
  if (!state_->grants.emplace(digest, std::move(grant)).second)
    throw std::runtime_error("Clan credential collision.");
  if (game->pending) state_->grants.erase(*game->pending);
  game->pending = digest;
  return credentials;
}

ClanAuthentication::Binding ClanAuthentication::Consume(
    const protocol::ClanCredentials& credentials) {
  Binding binding;
  if (credentials.passport.size() != 40) return binding;
  const auto digest = CredentialCrypto::Sha256(credentials.passport);
  std::lock_guard lock(state_->mutex);
  state_->Cleanup();
  const auto found = state_->grants.find(digest);
  if (found == state_->grants.end()) return binding;
  const auto grant = std::move(found->second);
  state_->grants.erase(found);  // One attempt, including mismatched metadata.
  const auto game = grant.game.lock();
  if (!game) return binding;
  game->pending.reset();
  const auto& metadata = grant.metadata;
  if (credentials.value_at0 != metadata.value_at0 ||
      credentials.text_at4 != metadata.text_at4 ||
      credentials.value_at28 != metadata.value_at28 ||
      credentials.value_at32 != metadata.value_at32 ||
      game->generation == std::numeric_limits<std::uint64_t>::max())
    return binding;
  binding.owner_ = std::make_shared<Inbox>();
  binding.inbox_ = binding.owner_;
  game->inbox = binding.inbox_;
  binding.state_ = state_;
  binding.game_ = game;
  binding.generation_ = ++game->generation;
  return binding;
}

std::optional<ClanIdentity> ClanAuthentication::Binding::Identity() const {
  if (!state_) return {};
  std::lock_guard lock(state_->mutex);
  const auto game = game_.lock();
  const auto inbox = inbox_.lock();
  if (!game || game->generation != generation_ || !inbox || inbox->failed)
    return {};
  return game->identity;
}
ClanAuthentication::Binding::Binding(const Binding& other)
    : state_(other.state_),
      game_(other.game_),
      generation_(other.generation_),
      inbox_(other.inbox_) {}
ClanAuthentication::Binding& ClanAuthentication::Binding::operator=(
    const Binding& other) {
  if (this != &other) {
    owner_.reset();
    state_ = other.state_;
    game_ = other.game_;
    generation_ = other.generation_;
    inbox_ = other.inbox_;
  }
  return *this;
}
std::unique_lock<std::mutex> ClanAuthentication::LockOperations() {
  return std::unique_lock(state_->operations);
}
void ClanAuthentication::UpdateScene(const GameLink& game,
                                     ClanGameScene scene) {
  if (!game || game->state.lock() != state_)
    throw std::invalid_argument("Missing trusted clan scene owner.");
  if ((scene.category != 20 && scene.category != 60 && scene.category != 70 &&
       scene.category != 80) ||
      (scene.type != 0 && scene.type != 1 && scene.type != 2 &&
       scene.type != 3 && scene.type != 4 && scene.type != 6))
    throw std::invalid_argument("Invalid trusted clan scene.");
  std::lock_guard lock(state_->mutex);
  if (game->scene == scene) return;
  if (game->plaza_epoch == std::numeric_limits<std::uint64_t>::max()) {
    game->plaza_available = false;
    game->scene.type = 0;
    throw std::runtime_error("Clan scene epoch exhausted.");
  }
  ++game->plaza_epoch;
  game->scene = scene;
  // Native clan-channel UI forbids disband/leave. Category80 is not an
  // ordinary plaza authority even when a future world uses scene type3.
  game->plaza_available =
      scene.type == 3 && scene.category != 80 && game->committed_category != 80;
}
void ClanAuthentication::PublishChannelCategory(
    std::span<const ClanIdentity> identities, std::uint32_t category) {
  std::lock_guard lock(state_->mutex);
  for (const auto& identity : identities) {
    const auto found = state_->games.find(identity.account_id);
    if (found == state_->games.end()) continue;
    const auto game = found->second.lock();
    if (!game || game->identity != identity) continue;
    game->committed_category = category;
    game->scene = {category, 0};
    game->plaza_available = false;
    if (game->plaza_epoch != std::numeric_limits<std::uint64_t>::max())
      ++game->plaza_epoch;
  }
}
std::vector<std::uint32_t> ClanAuthentication::ReadScenes(
    std::span<const std::uint32_t> characters) const {
  std::vector<std::uint32_t> result(characters.size());
  std::lock_guard lock(state_->mutex);
  for (const auto& [account, weak] : state_->games) {
    const auto game = weak.lock();
    if (!game) continue;
    for (std::size_t i = 0; i < characters.size(); ++i)
      if (characters[i] == game->identity.character_id)
        result[i] = game->scene.type;
  }
  return result;
}
ClanAuthentication::Binding ClanAuthentication::Find(
    std::uint32_t character_id) const {
  std::lock_guard lock(state_->mutex);
  for (const auto& [account, weak] : state_->games) {
    const auto game = weak.lock();
    if (!game || game->identity.character_id != character_id) continue;
    const auto inbox = game->inbox.lock();
    if (!inbox || inbox->failed) return {};
    Binding binding;
    binding.state_ = state_;
    binding.game_ = game;
    binding.generation_ = game->generation;
    binding.inbox_ = inbox;
    return binding;
  }
  return {};
}
std::optional<std::uint64_t> ClanAuthentication::Binding::PlazaEpoch() const {
  if (!state_) return {};
  std::lock_guard lock(state_->mutex);
  const auto game = game_.lock();
  const auto inbox = inbox_.lock();
  if (!game || game->generation != generation_ || !inbox || inbox->failed ||
      !game->plaza_available)
    return {};
  return game->plaza_epoch;
}
std::optional<ClanGameScene> ClanAuthentication::Binding::Scene() const {
  if (!state_) return {};
  std::lock_guard lock(state_->mutex);
  const auto game = game_.lock();
  const auto inbox = inbox_.lock();
  if (!game || game->generation != generation_ || !inbox || inbox->failed)
    return {};
  return game->scene;
}
bool ClanAuthentication::Binding::InClanChannel() const {
  if (!state_) return false;
  std::lock_guard lock(state_->mutex);
  const auto game = game_.lock();
  const auto inbox = inbox_.lock();
  return game && game->generation == generation_ && inbox && !inbox->failed &&
         (game->committed_category == 80 || game->scene.category == 80);
}
bool ClanAuthentication::Binding::SameConnection(const Binding& other) const {
  if (!state_ || state_ != other.state_) return false;
  std::lock_guard lock(state_->mutex);
  const auto game = game_.lock();
  const auto inbox = inbox_.lock();
  return game && game == other.game_.lock() &&
         game->generation == generation_ && generation_ == other.generation_ &&
         inbox && !inbox->failed && inbox == other.inbox_.lock();
}
void ClanAuthentication::InvalidateMembership() {
  std::lock_guard lock(state_->mutex);
  ++state_->membership_revision;  // Unsigned wrap cannot throw after commit.
}
std::uint64_t ClanAuthentication::MembershipRevision() const {
  std::lock_guard lock(state_->mutex);
  return state_->membership_revision;
}
void ClanAuthentication::Publish(std::span<const ClanDelivery> deliveries) {
  std::lock_guard lock(state_->mutex);
  for (const auto& delivery : deliveries) {
    for (const auto& [account, weak] : state_->games) {
      const auto game = weak.lock();
      if (!game || !std::binary_search(delivery.characters.begin(),
                                       delivery.characters.end(),
                                       game->identity.character_id))
        continue;
      const auto inbox = game->inbox.lock();
      if (!inbox || inbox->failed) continue;
      if (!delivery.packets || delivery.packets->empty() ||
          inbox->count == inbox->batches.size()) {
        inbox->failed = true;  // Never silently discard a committed transition.
        continue;
      }
      inbox->batches[inbox->count++] = delivery.packets;
    }
  }
}
std::optional<std::vector<protocol::ClanPacket>>
ClanAuthentication::Binding::TakeNotifications() {
  if (!state_) return {};
  std::lock_guard lock(state_->mutex);
  const auto game = game_.lock();
  const auto inbox = inbox_.lock();
  if (!game || game->generation != generation_ || !inbox || inbox->failed)
    return {};
  std::vector<protocol::ClanPacket> packets;
  for (std::size_t i = 0; i < inbox->count; ++i) {
    const auto& batch = *inbox->batches[i];
    packets.insert(packets.end(), batch.begin(), batch.end());
  }
  for (std::size_t i = 0; i < inbox->count; ++i) inbox->batches[i].reset();
  inbox->count = 0;
  return packets;
}
}  // namespace bubble_fighter::server::features::clans
