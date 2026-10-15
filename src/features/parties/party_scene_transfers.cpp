#include "features/parties/party_scene_transfers.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "protocol/rooms/join_room_response.h"

namespace bubble_fighter::server::features::parties {
PartySceneConnection::PartySceneConnection(
    std::shared_ptr<PartySceneTransfers> registry, std::uint64_t token) noexcept
    : registry_(std::move(registry)), token_(token) {}
PartySceneConnection::PartySceneConnection(
    PartySceneConnection&& other) noexcept
    : registry_(std::move(other.registry_)),
      token_(std::exchange(other.token_, 0)) {}
PartySceneConnection& PartySceneConnection::operator=(
    PartySceneConnection&& other) noexcept {
  if (this != &other) {
    Release();
    registry_ = std::move(other.registry_);
    token_ = std::exchange(other.token_, 0);
  }
  return *this;
}
PartySceneConnection::~PartySceneConnection() { Release(); }
PartySceneConnection::operator bool() const noexcept {
  return registry_ != nullptr;
}
void PartySceneConnection::Release() noexcept {
  if (registry_) registry_->Cancel(token_, true);
  registry_.reset();
  token_ = 0;
}
bool PartySceneConnection::Begin(const PartyMembership& leader,
                                 PartySceneRequest request, Time now) {
  return registry_ && registry_->Begin(token_, leader, std::move(request), now);
}
bool PartySceneConnection::Pending() const {
  return registry_ && registry_->Pending(token_);
}
std::optional<PartySceneWork> PartySceneConnection::Work(Time now) {
  return registry_ ? registry_->Work(token_, now) : std::nullopt;
}
bool PartySceneConnection::Prepare(std::uint64_t id,
                                   std::shared_ptr<PartyScenePrepared> prepared,
                                   Time now) {
  return registry_ && registry_->Prepare(token_, id, std::move(prepared), now);
}
void PartySceneConnection::Cancel() noexcept {
  if (registry_) registry_->Cancel(token_, false);
}
std::optional<PartySceneOutcome> PartySceneConnection::Take(Time now) {
  return registry_ ? registry_->Take(token_, now) : std::nullopt;
}
PartySceneTransfers::PartySceneTransfers(
    std::shared_ptr<PartyRegistry> parties,
    std::shared_ptr<rooms::RoomRegistry> rooms,
    std::shared_ptr<lobby::PlazaRegistry> plazas,
    clans::ClanPlazaAuthority clan_plaza_authority)
    : parties_(std::move(parties)),
      rooms_(std::move(rooms)),
      plazas_(std::move(plazas)),
      clan_plaza_authority_(std::move(clan_plaza_authority)) {
  if (!parties_ || !rooms_ || !plazas_)
    throw std::invalid_argument("Missing party scene dependencies.");
}
PartySceneConnection PartySceneTransfers::Register(
    const PartyMembership& member) {
  if (!parties_->Owns(member))
    throw std::invalid_argument("Foreign party scene connection.");
  const auto self = shared_from_this();
  std::lock_guard lock(mutex_);
  if (!connections_.emplace(member.token_, nullptr).second)
    throw std::invalid_argument("Duplicate party scene connection.");
  return PartySceneConnection(self, member.token_);
}
bool PartySceneTransfers::Begin(std::uint64_t token,
                                const PartyMembership& leader,
                                PartySceneRequest request, Time now) {
  if (request.kind == PartySceneKind::kCreateRoom)
    rooms::RoomCreation::ValidateRequest(request.create);
  std::lock_guard lock(mutex_);
  if (!parties_->Owns(leader) || leader.token_ != token ||
      !connections_.contains(token) || connections_.at(token))
    return false;
  auto reservation = parties_->ReserveScene(
      leader, request.kind == PartySceneKind::kClanPlaza);
  if (!reservation) return false;
  for (const auto& member : reservation->Members())
    if (!connections_.contains(member.token) || connections_.at(member.token))
      return false;
  if (request.kind == PartySceneKind::kClanPlaza) {
    if (!clan_plaza_authority_) return false;
    std::vector<characters::CharacterRecord> members;
    for (const auto& member : reservation->Members())
      members.push_back(member.character);
    if (!clan_plaza_authority_(request.clan_id, members,
                               [](const auto&) { return true; }))
      return false;
  }
  if (next_id_ == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("Party scene identity space exhausted.");
  auto operation = std::make_shared<Operation>();
  operation->id = next_id_++;
  operation->request = std::move(request);
  operation->deadline = now + std::chrono::seconds(5);
  operation->reservation = std::move(reservation);
  if (operation->request.kind == PartySceneKind::kCreateRoom) {
    operation->refusal.push_back(protocol::CreateRoomResponse{
        0, static_cast<std::uint32_t>(protocol::CreateRoomResult::kFailed), 0}
                                     .Encode());
  } else if (operation->request.kind == PartySceneKind::kJoinRoom) {
    operation->refusal.push_back(protocol::JoinRoomResponse{
        static_cast<std::uint32_t>(protocol::JoinRoomResult::kNoMatchingRoom),
        0}
                                     .Encode());
  } else {
    operation->refusal.push_back(
        chat::SceneChat::Notice(u"队伍移动未完成，请等待所有成员就绪后重试。"));
  }
  for (const auto& member : operation->reservation->Members()) {
    Participant participant;
    participant.identity = member;
    participant.packets = std::make_shared<rooms::RoomPacketQueue>();
    operation->participants.push_back(std::move(participant));
  }
  // No throwing operation after publishing the shared command to a connection.
  for (const auto& member : operation->participants)
    connections_.at(member.identity.token) = operation;
  return true;
}
PartySceneTransfers::Participant& PartySceneTransfers::Find(
    Operation& operation, std::uint64_t token) {
  const auto found =
      std::find_if(operation.participants.begin(), operation.participants.end(),
                   [token](const auto& participant) {
                     return participant.identity.token == token;
                   });
  if (found == operation.participants.end())
    throw std::logic_error("Missing party scene participant.");
  return *found;
}
void PartySceneTransfers::Expire(Operation& operation, Time now) noexcept {
  if (!operation.finished && now >= operation.deadline)
    operation.finished = true;
}
bool PartySceneTransfers::Pending(std::uint64_t token) const {
  std::lock_guard lock(mutex_);
  const auto found = connections_.find(token);
  return found != connections_.end() && found->second != nullptr;
}
std::optional<PartySceneWork> PartySceneTransfers::Work(std::uint64_t token,
                                                        Time now) {
  std::lock_guard lock(mutex_);
  const auto found = connections_.find(token);
  if (found == connections_.end() || !found->second) return {};
  auto& operation = *found->second;
  Expire(operation, now);
  if (operation.finished || Find(operation, token).prepared) return {};
  return PartySceneWork{operation.id, operation.request};
}
bool PartySceneTransfers::Prepare(std::uint64_t token, std::uint64_t id,
                                  std::shared_ptr<PartyScenePrepared> prepared,
                                  Time now) {
  std::lock_guard lock(mutex_);
  const auto found = connections_.find(token);
  if (found == connections_.end() || !found->second || found->second->id != id)
    return false;
  auto& operation = *found->second;
  Expire(operation, now);
  if (operation.finished) return false;
  auto& participant = Find(operation, token);
  const auto& identity = participant.identity.character;
  if (!prepared || participant.prepared ||
      prepared->character.account_id != identity.account_id ||
      prepared->character.character_id != identity.character_id ||
      prepared->character.character_name != identity.character_name ||
      prepared->equipment.key != identity.character_id) {
    operation.finished = true;
    return false;
  }
  participant.prepared = std::move(prepared);
  if (std::all_of(operation.participants.begin(), operation.participants.end(),
                  [](const auto& p) { return p.prepared != nullptr; })) {
    try {
      Commit(operation);
    } catch (...) {
      operation.finished = true;
      // Encoding/admission failure cancels the whole operation. The last
      // participant to prepare is not blamed for another member's snapshot.
    }
  }
  return true;
}
void PartySceneTransfers::Cancel(std::uint64_t token,
                                 bool disconnect) noexcept {
  std::lock_guard lock(mutex_);
  const auto found = connections_.find(token);
  if (found == connections_.end()) return;
  if (found->second) {
    auto& operation = *found->second;
    operation.finished = true;  // Committed outcomes cannot be rolled back.
    if (disconnect) {
      auto& participant = Find(operation, token);
      participant.room.Release();
      participant.plaza.Release();
      participant.prepared.reset();
    }
  }
  if (disconnect) connections_.erase(found);
}
std::optional<PartySceneOutcome> PartySceneTransfers::Take(std::uint64_t token,
                                                           Time now) {
  std::lock_guard lock(mutex_);
  const auto found = connections_.find(token);
  if (found == connections_.end() || !found->second) return {};
  auto& operation = *found->second;
  Expire(operation, now);
  if (!operation.finished) return {};
  auto& participant = Find(operation, token);
  PartySceneOutcome result;
  result.kind = operation.request.kind;
  result.committed = operation.committed;
  result.prepared = std::move(participant.prepared);
  result.room = std::move(participant.room);
  result.plaza = std::move(participant.plaza);
  result.packets = std::move(participant.packets);
  if (!operation.committed &&
      operation.participants.front().identity.token == token)
    result.refusal = std::move(operation.refusal);
  found->second.reset();
  return result;
}
void PartySceneTransfers::Commit(Operation& operation) {
  operation.committed = operation.reservation->Commit([&] {
    if (operation.request.kind != PartySceneKind::kClanPlaza)
      return CommitScene(operation);
    if (!clan_plaza_authority_) return false;
    std::vector<characters::CharacterRecord> members;
    for (const auto& participant : operation.participants)
      members.push_back(participant.prepared->character);
    return clan_plaza_authority_(
        operation.request.clan_id, members,
        [&](const auto& scope) { return CommitScene(operation, scope); });
  });
  operation.finished = true;
  if (operation.committed) {
    for (auto& participant : operation.participants)
      participant.prepared->source.Release();
  }
}
bool PartySceneTransfers::CommitScene(Operation& operation,
                                      const clans::ClanPlazaScope& scope) {
  const auto kind = operation.request.kind;
  for (const auto& participant : operation.participants) {
    const auto& data = *participant.prepared;
    const auto source = plazas_->Capture(data.character.account_id);
    if ((source ? source->token : 0) != data.source.Token()) return false;
    if ((kind == PartySceneKind::kCreateRoom ||
         kind == PartySceneKind::kJoinRoom) &&
        !source)
      return false;
  }
  if (kind == PartySceneKind::kPlaza || kind == PartySceneKind::kLeaveRoom ||
      kind == PartySceneKind::kClanPlaza) {
    std::vector<lobby::PlazaEntry> entries;
    std::vector<rooms::RoomDeparture> departures;
    for (const auto& p : operation.participants) {
      auto& data = *p.prepared;
      entries.push_back({{data.member, data.character, data.equipment},
                         data.character.account_id,
                         p.packets->Sink(),
                         data.source.Token()});
      if (data.source_room)
        departures.push_back(
            {&data.source_room,
             {data.character.account_id, data.character.character_id}});
    }
    const auto admit = [&] {
      auto joined =
          plazas_->EnterGroup(operation.participants.front().prepared->envelope,
                              std::move(entries), {}, scope);
      if (joined.status != lobby::PlazaAdmissionStatus::kEntered) return false;
      for (std::size_t i = 0; i < joined.members.size(); ++i)
        operation.participants[i].plaza = std::move(joined.members[i]);
      return true;
    };
    return (kind == PartySceneKind::kLeaveRoom ||
            (kind == PartySceneKind::kClanPlaza && !departures.empty()))
               ? rooms_->DepartGroup(departures, admit)
               : admit();
  }
  if (kind == PartySceneKind::kMenu) {
    // Every private menu snapshot was fully encoded by its owning session.
    // Allocate all sinks before any publication.
    std::vector<rooms::RoomPacketSink> sinks;
    for (const auto& p : operation.participants) {
      if (p.prepared->menu_packets.empty()) return false;
      sinks.push_back(p.packets->Sink());
    }
    for (std::size_t i = 0; i < sinks.size(); ++i)
      sinks[i](std::move(operation.participants[i].prepared->menu_packets));
    return true;
  }
  if (kind == PartySceneKind::kCreateRoom) {
    std::vector<rooms::RoomCreationInput> inputs;
    for (const auto& p : operation.participants) {
      const auto& data = *p.prepared;
      inputs.push_back({data.character.account_id, data.character, data.member,
                        data.equipment, p.packets->Sink()});
    }
    const rooms::RoomCreationPolicy policy{
        {rooms::RoomCreation::kStandardPvpMode},
        {0},
        rooms::RoomCreation::kDialogTitleCodeUnits,
        rooms::RoomCreation::kDialogPasswordCodeUnits};
    auto created = rooms::RoomCreation::CreateGroup(
        *rooms_, policy, operation.request.create, std::move(inputs));
    if (created.members.empty()) {
      operation.refusal = std::move(created.responses);
      return false;
    }
    for (std::size_t i = 0; i < created.members.size(); ++i)
      operation.participants[i].room = std::move(created.members[i]);
    return true;
  }
  const auto& request = operation.request.join;
  if (request.room_id == 0 || request.room_id > rooms::RoomRegistry::kMaxRoomId)
    return false;
  std::vector<rooms::RoomAdmissionEntry> entries;
  for (const auto& p : operation.participants) {
    const auto& data = *p.prepared;
    rooms::RoomOccupant member;
    member.account_id = data.character.account_id;
    member.character = data.character;
    member.member = data.member;
    member.equipment = data.equipment;
    std::vector<protocol::TcpPacket> prefix;
    if (entries.empty())
      prefix.push_back(protocol::JoinRoomResponse{}.Encode());
    entries.push_back(
        {std::move(member), p.packets->Sink(), std::move(prefix)});
  }
  auto joined = rooms_->JoinGroup(static_cast<std::uint16_t>(request.room_id),
                                  request.password, std::move(entries));
  if (joined.status != rooms::RoomJoinStatus::kJoined) {
    using Result = protocol::JoinRoomResult;
    const auto reason =
        joined.status == rooms::RoomJoinStatus::kPlaying ? Result::kGameStarted
        : joined.status == rooms::RoomJoinStatus::kFull  ? Result::kRoomFull
        : joined.status == rooms::RoomJoinStatus::kWrongPassword
            ? Result::kWrongPassword
            : Result::kNoMatchingRoom;
    operation.refusal = {
        protocol::JoinRoomResponse{static_cast<std::uint32_t>(reason), 0}
            .Encode()};
    return false;
  }
  for (std::size_t i = 0; i < joined.members.size(); ++i)
    operation.participants[i].room = std::move(joined.members[i]);
  return true;
}
}  // namespace bubble_fighter::server::features::parties
