#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "features/lobby/plaza_registry.h"
#include "protocol/rooms/room_member_left_notification.h"
#include "protocol/scenes/scene_descriptor_notification.h"

namespace bubble_fighter::server::features::lobby {
bool PlazaRegistry::Contains(std::uint16_t scene, std::uint64_t account,
                             std::uint64_t token) const {
  const auto found = scenes_.find(scene);
  if (found == scenes_.end()) return false;
  const auto member = found->second.members.find(account);
  return member != found->second.members.end() && member->second.token == token;
}
std::uint64_t PlazaRegistry::NextToken() {
  if (next_token_ == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("Plaza identity space exhausted.");
  return next_token_++;
}
std::optional<PlazaTarget> PlazaRegistry::Capture(std::uint64_t account) const {
  std::lock_guard lock(mutex_);
  for (const auto& [id, scene] : scenes_) {
    const auto found = scene.members.find(account);
    if (found != scene.members.end())
      return PlazaTarget{id, account, found->second.token, scene.scope.clan_id};
  }
  return {};
}

PlazaAdmission PlazaRegistry::EnterGroup(
    protocol::GotoRoomNotification envelope, std::vector<PlazaEntry> entries,
    std::optional<PlazaTarget> target, clans::ClanPlazaScope scope) {
  if ((!scope.clan_id && !scope.name.empty()) ||
      (scope.clan_id && (scope.clan_id > 0x7FFFFFFFU || scope.name.empty() ||
                         scope.name.size() > 12)))
    throw std::invalid_argument("Invalid trusted clan plaza scope.");
  if (entries.empty() || entries.size() > kCapacity)
    throw std::invalid_argument("Invalid plaza admission group size.");
  std::set<std::uint64_t> accounts, characters;
  for (const auto& entry : entries) {
    if (!entry.account || !entry.player.character ||
        entry.player.character->account_id != entry.account ||
        !entry.player.equipment ||
        entry.player.equipment->key != entry.player.character->character_id ||
        !entry.sink || !accounts.insert(entry.account).second ||
        !characters.insert(entry.player.character->character_id).second)
      throw std::invalid_argument("Invalid authenticated plaza member.");
  }
  auto self = shared_from_this();
  std::lock_guard lock(mutex_);
  if (target && (!Contains(target->scene, target->account, target->token) ||
                 target->clan_id != scope.clan_id ||
                 scenes_.at(target->scene).scope.clan_id != scope.clan_id))
    return {PlazaAdmissionStatus::kStale, {}};

  // Validate the source capabilities before staging departures. An old lease
  // must never move or remove a replacement connection with the same account.
  std::map<std::uint64_t, std::uint16_t> sources;
  for (const auto& entry : entries) {
    for (const auto& [id, scene] : scenes_) {
      for (const auto& [account, member] : scene.members) {
        if (account == entry.account) {
          if (!entry.source_token || entry.source_token != member.token ||
              member.player.character->character_id !=
                  entry.player.character->character_id)
            return {PlazaAdmissionStatus::kStale, {}};
          sources.emplace(account, id);
        } else if (member.player.character->character_id ==
                   entry.player.character->character_id) {
          return {PlazaAdmissionStatus::kStale, {}};
        }
      }
    }
    if (entry.source_token && !sources.contains(entry.account))
      return {PlazaAdmissionStatus::kStale, {}};
  }
  const auto fits = [&](const Scene& scene) {
    const auto leaving = std::count_if(
        scene.members.begin(), scene.members.end(),
        [&](const auto& member) { return accounts.contains(member.first); });
    return scene.members.size() - leaving + entries.size() <= kCapacity;
  };
  std::uint16_t destination = target ? target->scene : 0;
  if (target && !fits(scenes_.at(destination)))
    return {PlazaAdmissionStatus::kFull, {}};
  if (!target) {
    for (const auto& [id, scene] : scenes_) {
      if (scene.scope.clan_id == scope.clan_id && fits(scene)) {
        destination = id;
        break;
      }
    }
    if (!destination) {
      for (std::uint16_t id = 1; id <= 512; ++id) {
        if (!scenes_.contains(id)) {
          destination = id;
          break;
        }
      }
    }
    if (!destination) return {PlazaAdmissionStatus::kFull, {}};
  }
  // Only copy affected scenes. Map nodes for a new destination are allocated
  // here, so the final commit requires no allocation or packet encoding.
  std::map<std::uint16_t, Scene> planned;
  for (const auto& [account, id] : sources) {
    if (!planned.contains(id)) planned.emplace(id, scenes_.at(id));
    planned.at(id).members.erase(account);
  }
  if (!planned.contains(destination)) {
    const auto found = scenes_.find(destination);
    planned.emplace(destination, found == scenes_.end()
                                     ? Scene{std::move(envelope), {}}
                                     : found->second);
  }
  auto& admitted = planned.at(destination);
  admitted.scope = std::move(scope);
  admitted.envelope.descriptor.short_at0 = destination;
  for (auto& entry : entries) {
    std::array<bool, kCapacity> used{};
    for (const auto& [account, member] : admitted.members)
      used.at(member.slot) = true;
    const auto slot = static_cast<std::uint32_t>(
        std::find(used.begin(), used.end(), false) - used.begin());
    admitted.members.emplace(entry.account, Member{slot,
                                                   std::move(entry.player),
                                                   std::move(entry.sink),
                                                   {},
                                                   NextToken()});
  }
  struct Publication {
    rooms::RoomPacketSink sink;
    std::vector<protocol::TcpPacket> packets;
  };
  std::vector<Publication> publications;
  std::vector<protocol::TcpPacket> updates;
  for (const auto& entry : entries) {
    const auto snapshot = Snapshot(admitted, entry.account);
    updates.push_back(Update(snapshot));
    std::vector<protocol::TcpPacket> own{snapshot.Encode()};
    for (const auto& [account, member] : admitted.members) {
      if (account != entry.account && member.movement)
        own.push_back(protocol::PlazaMovementNotification{
            static_cast<std::uint32_t>(member.player.character->character_id),
            *member.movement}
                          .Encode());
    }
    publications.push_back(
        {admitted.members.at(entry.account).sink, std::move(own)});
  }
  for (const auto& [id, scene] : planned) {
    if (scene.members.empty()) continue;
    std::vector<protocol::TcpPacket> changes;
    const auto previous = scenes_.find(id);
    if (previous != scenes_.end()) {
      for (const auto& [account, member] : previous->second.members) {
        if (accounts.contains(account)) {
          protocol::RoomMemberLeftNotification left;
          left.slot = member.slot;
          left.team = previous->second.envelope.team;
          changes.push_back(left.Encode());
        }
      }
    }
    if (id == destination)
      changes.insert(changes.end(), updates.begin(), updates.end());
    changes.push_back(protocol::SceneDescriptorNotification{
        Snapshot(scene, scene.members.begin()->first).descriptor}
                          .Encode());
    for (const auto& [account, member] : scene.members) {
      if (!accounts.contains(account))
        publications.push_back({member.sink, changes});
    }
  }
  PlazaAdmission result{PlazaAdmissionStatus::kEntered, {}};
  result.members.reserve(entries.size());
  // Scene move assignment and node insertion cannot allocate. All failure-prone
  // identity validation, encoders, callback copies and vector growth precede
  // it.
  static_assert(std::is_nothrow_move_assignable_v<Scene>);
  while (!planned.empty()) {
    auto next = planned.begin();
    const auto existing = scenes_.find(next->first);
    if (next->second.members.empty()) {
      if (existing != scenes_.end()) scenes_.erase(existing);
      planned.erase(next);
    } else if (existing == scenes_.end()) {
      scenes_.insert(planned.extract(next));
    } else {
      existing->second = std::move(next->second);
      planned.erase(next);
    }
  }
  for (const auto& entry : entries) {
    result.members.push_back(
        PlazaLease(self, destination, entry.account,
                   scenes_.at(destination).members.at(entry.account).token,
                   scenes_.at(destination).scope.clan_id));
  }
  for (auto& publication : publications) {
    try {
      publication.sink(std::move(publication.packets));
    } catch (...) {
      // One failed connection cannot split committed membership or peer
      // delivery.
    }
  }
  return result;
}
}  // namespace bubble_fighter::server::features::lobby
