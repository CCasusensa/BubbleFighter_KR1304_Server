#include "features/channels/party_channel_transfers.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "features/channels/channel_worlds.h"

namespace bubble_fighter::server::features::channels {
PartyChannelConnection::PartyChannelConnection(
    std::shared_ptr<PartyChannelTransfers> registry,
    std::uint64_t token) noexcept
    : registry_(std::move(registry)), token_(token) {}
PartyChannelConnection::PartyChannelConnection(
    PartyChannelConnection&& other) noexcept
    : registry_(std::move(other.registry_)),
      token_(std::exchange(other.token_, 0)) {}
PartyChannelConnection& PartyChannelConnection::operator=(
    PartyChannelConnection&& other) noexcept {
  if (this != &other) {
    Release();
    registry_ = std::move(other.registry_);
    token_ = std::exchange(other.token_, 0);
  }
  return *this;
}
PartyChannelConnection::~PartyChannelConnection() { Release(); }
void PartyChannelConnection::Release() noexcept {
  if (registry_) registry_->Cancel(token_, true);
  registry_.reset();
  token_ = 0;
}
void PartyChannelConnection::Enable() {
  if (!registry_) return;
  std::lock_guard lock(registry_->mutex_);
  registry_->connections_.at(token_).capable = true;
}
void PartyChannelConnection::SetCategory(std::uint32_t category) {
  if (!registry_) return;
  std::lock_guard lock(registry_->mutex_);
  registry_->connections_.at(token_).category = category;
}
bool PartyChannelConnection::Pending() const {
  if (!registry_) return false;
  std::lock_guard lock(registry_->mutex_);
  return registry_->connections_.at(token_).operation != nullptr;
}
bool PartyChannelConnection::Irreversible() const {
  if (!registry_) return false;
  std::lock_guard lock(registry_->mutex_);
  const auto& op = registry_->connections_.at(token_).operation;
  return op && op->irreversible && !op->committed;
}
std::uint32_t PartyChannelConnection::Refusal() const {
  if (!registry_) return 2;
  std::lock_guard lock(registry_->mutex_);
  return registry_->connections_.at(token_).refusal;
}
bool PartyChannelConnection::Begin(
    const parties::PartyMembership& leader,
    std::shared_ptr<const ChannelWorld> source,
    std::shared_ptr<const ChannelWorld> destination, Time now) {
  return registry_ && registry_->Begin(token_, leader, std::move(source),
                                       std::move(destination), now);
}
ChannelMoveStatus PartyChannelConnection::Status(Time now) const {
  return registry_ ? registry_->Status(token_, now) : ChannelMoveStatus{};
}
bool PartyChannelConnection::Prepare(
    std::uint64_t id, std::shared_ptr<ChannelMovePrepared> prepared, Time now) {
  return registry_ && registry_->Prepare(token_, id, std::move(prepared), now);
}
bool PartyChannelConnection::Signal(ChannelMoveSignal signal, Time now) {
  return registry_ && registry_->Signal(token_, signal, now);
}
void PartyChannelConnection::Cancel() noexcept {
  if (registry_) registry_->Cancel(token_, false);
}
std::optional<ChannelMoveOutcome> PartyChannelConnection::Take(Time now) {
  return registry_ ? registry_->Take(token_, now) : std::nullopt;
}
PartyChannelTransfers::PartyChannelTransfers(
    std::shared_ptr<parties::PartyRegistry> parties)
    : parties_(std::move(parties)) {
  if (!parties_) throw std::invalid_argument("Missing channel party registry.");
}
PartyChannelConnection PartyChannelTransfers::Register(
    const parties::PartyMembership& member, std::uint32_t category) {
  if (!parties_->Owns(member))
    throw std::invalid_argument("Foreign party channel connection.");
  auto self = shared_from_this();
  std::lock_guard lock(mutex_);
  if (!connections_.emplace(member.token_, Connection{category}).second)
    throw std::invalid_argument("Duplicate party channel connection.");
  return {std::move(self), member.token_};
}
PartyChannelTransfers::Participant& PartyChannelTransfers::Find(
    Operation& op, std::uint64_t token) {
  const auto found = std::find_if(
      op.participants.begin(), op.participants.end(),
      [token](const auto& p) { return p.identity.token == token; });
  if (found == op.participants.end())
    throw std::logic_error("Missing party channel participant.");
  return *found;
}
void PartyChannelTransfers::Expire(Operation& op, Time now) noexcept {
  if (!op.finished && now >= op.deadline) op.finished = true;
}
bool PartyChannelTransfers::Begin(
    std::uint64_t token, const parties::PartyMembership& leader,
    std::shared_ptr<const ChannelWorld> source,
    std::shared_ptr<const ChannelWorld> destination, Time now) {
  std::lock_guard lock(mutex_);
  if (connections_.contains(token)) connections_.at(token).refusal = 2;
  if (!source || !destination || source == destination ||
      !parties_->Owns(leader) || leader.token_ != token ||
      !connections_.contains(token))
    return false;
  auto reservation = parties_->ReserveScene(leader);
  if (!reservation) return false;
  for (const auto& member : reservation->Members()) {
    const auto found = connections_.find(member.token);
    if (found == connections_.end() || !found->second.capable ||
        found->second.operation || found->second.category != source->category)
      return false;
  }
  std::vector<characters::CharacterRecord> identities;
  for (const auto& member : reservation->Members())
    identities.push_back(member.character);
  const auto decision = destination->Authorize(identities);
  if (decision.result != 0) {
    connections_.at(token).refusal = decision.result;
    return false;
  }
  if (next_id_ == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("Party channel identity space exhausted.");
  auto op = std::make_shared<Operation>();
  op->id = next_id_++;
  op->deadline = now + std::chrono::seconds(10);
  op->source = std::move(source);
  op->destination = std::move(destination);
  op->reservation = std::move(reservation);
  for (const auto& member : op->reservation->Members()) {
    Participant p;
    p.identity = member;
    op->participants.push_back(std::move(p));
  }
  for (const auto& member : op->participants)
    connections_.at(member.identity.token).operation = op;
  return true;
}
ChannelMoveStatus PartyChannelTransfers::Status(std::uint64_t token, Time now) {
  std::lock_guard lock(mutex_);
  const auto& ptr = connections_.at(token).operation;
  if (!ptr) return {};
  auto& op = *ptr;
  Expire(op, now);
  auto& p = Find(op, token);
  ChannelMoveStatus result{ChannelMovePhase::kWaiting,
                           op.id,
                           op.destination->category,
                           op.participants.front().identity.token == token,
                           op.irreversible,
                           op.destination};
  if (op.finished)
    result.phase = op.committed      ? ChannelMovePhase::kCommitted
                   : op.irreversible ? ChannelMovePhase::kClosed
                                     : ChannelMovePhase::kCancelled;
  else if (!p.prepared)
    result.phase = ChannelMovePhase::kPreparing;
  else if (!p.offered &&
           std::all_of(op.participants.begin(), op.participants.end(),
                       [](const auto& v) { return v.prepared != nullptr; }))
    result.phase = ChannelMovePhase::kOffer;
  else if (p.offered && !p.entered)
    result.phase = ChannelMovePhase::kConnecting;
  else if (!p.switched &&
           std::all_of(op.participants.begin(), op.participants.end(),
                       [](const auto& v) { return v.entered; }))
    result.phase = ChannelMovePhase::kSwitch;
  return result;
}
bool PartyChannelTransfers::Prepare(
    std::uint64_t token, std::uint64_t id,
    std::shared_ptr<ChannelMovePrepared> prepared, Time now) {
  std::lock_guard lock(mutex_);
  const auto& ptr = connections_.at(token).operation;
  if (!ptr || ptr->id != id) return false;
  auto& op = *ptr;
  Expire(op, now);
  if (op.finished) return false;
  auto& p = Find(op, token);
  if (p.prepared || !prepared || !prepared->packets ||
      prepared->character.account_id != p.identity.character.account_id ||
      prepared->character.character_id != p.identity.character.character_id ||
      !op.destination->Allows(prepared->character))
    return false;
  p.prepared = std::move(prepared);
  return true;
}
bool PartyChannelTransfers::Signal(std::uint64_t token,
                                   ChannelMoveSignal signal, Time now) {
  std::lock_guard lock(mutex_);
  const auto& ptr = connections_.at(token).operation;
  if (!ptr) throw std::logic_error("Party channel signal without operation.");
  auto& op = *ptr;
  Expire(op, now);
  if (op.finished) return false;
  auto& p = Find(op, token);
  if (signal == ChannelMoveSignal::kOffered) {
    if (p.offered ||
        !std::all_of(op.participants.begin(), op.participants.end(),
                     [](const auto& v) { return v.prepared != nullptr; }))
      throw std::logic_error("Premature party channel offer.");
    p.offered = true;
  } else if (signal == ChannelMoveSignal::kEntered) {
    if (!p.offered || p.entered)
      throw std::logic_error("Invalid party channel entry.");
    p.entered = true;
  } else if (signal == ChannelMoveSignal::kSwitched) {
    if (p.switched ||
        !std::all_of(op.participants.begin(), op.participants.end(),
                     [](const auto& v) { return v.entered; }))
      throw std::logic_error("Premature party channel switch.");
    p.switched = true;
    op.irreversible = true;
  } else {
    if (!p.switched || p.acknowledged)
      throw std::logic_error("Invalid party channel acknowledgement.");
    p.acknowledged = true;
    if (std::all_of(op.participants.begin(), op.participants.end(),
                    [](const auto& v) { return v.acknowledged; })) {
      try {
        op.committed = op.reservation->Commit([&] { return Commit(op); });
      } catch (...) {
        op.committed = false;
      }
      op.finished = true;
      if (op.committed)
        for (auto& member : op.participants) member.prepared->source.Release();
    }
  }
  return true;
}
bool PartyChannelTransfers::Commit(Operation& op) {
  std::vector<lobby::PlazaEntry> entries;
  std::vector<rooms::RoomDeparture> departures;
  for (const auto& p : op.participants) {
    auto& data = *p.prepared;
    const auto source = op.source->plazas->Capture(data.character.account_id);
    if ((source ? source->token : 0) != data.source.Token()) return false;
    entries.push_back({{data.member, data.character, data.equipment},
                       data.character.account_id,
                       data.packets->Sink(),
                       0});
    if (data.source_room)
      departures.push_back(
          {&data.source_room,
           {data.character.account_id, data.character.character_id}});
  }
  auto admit = [&] {
    auto joined = op.destination->plazas->EnterGroup(
        op.participants.front().prepared->envelope, std::move(entries));
    if (joined.status != lobby::PlazaAdmissionStatus::kEntered) return false;
    for (std::size_t i = 0; i < joined.members.size(); ++i)
      op.participants[i].plaza = std::move(joined.members[i]);
    return true;
  };
  std::vector<characters::CharacterRecord> identities;
  for (const auto& p : op.participants)
    identities.push_back(p.prepared->character);
  return op.destination
             ->Authorize(
                 identities,
                 [&] {
                   return departures.empty() ? admit()
                                             : op.source->rooms->DepartGroup(
                                                   departures, admit);
                 },
                 true)
             .result == 0;
}
void PartyChannelTransfers::Cancel(std::uint64_t token,
                                   bool disconnect) noexcept {
  std::lock_guard lock(mutex_);
  const auto found = connections_.find(token);
  if (found == connections_.end()) return;
  if (found->second.operation) {
    auto& op = *found->second.operation;
    op.finished = true;
    if (disconnect) {
      auto& p = Find(op, token);
      p.plaza.Release();
      p.prepared.reset();
    }
  }
  if (disconnect) connections_.erase(found);
}
std::optional<ChannelMoveOutcome> PartyChannelTransfers::Take(
    std::uint64_t token, Time now) {
  std::lock_guard lock(mutex_);
  auto& ptr = connections_.at(token).operation;
  if (!ptr) return {};
  auto& op = *ptr;
  Expire(op, now);
  if (!op.finished) return {};
  auto& p = Find(op, token);
  ChannelMoveOutcome result{op.committed, op.irreversible && !op.committed,
                            std::move(p.prepared), std::move(p.plaza)};
  if (op.committed) connections_.at(token).category = op.destination->category;
  ptr.reset();
  return result;
}
}  // namespace bubble_fighter::server::features::channels
