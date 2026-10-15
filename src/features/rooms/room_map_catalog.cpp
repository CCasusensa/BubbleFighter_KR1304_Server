#include "features/rooms/room_map_catalog.h"

#include <algorithm>

#include "features/rooms/generated/room_map_catalog_data.h"

namespace bubble_fighter::server::features::rooms {
std::span<const RoomMapDefinition> RoomMapCatalog::Maps() {
  return kRoomMapDefinitions;
}
const RoomMapDefinition* RoomMapCatalog::Find(std::u16string_view key) {
  const auto maps = Maps();
  const auto found =
      std::find_if(maps.begin(), maps.end(),
                   [&](const auto& map) { return map.key == key; });
  return found == maps.end() ? nullptr : &*found;
}

void RoomMapCatalog::DefaultRandom(
    protocol::SessionConfiguration& configuration) {
  // Project creation policy, matching 0x113BFA0 / 0x113C3E0 scene defaults.
  configuration.texts_at44 = {u"Random", u"", u"", u""};
  configuration.value_at140 = 1;
  configuration.texts_at144 = {u"Random", u"", u"Random", u"", u"Random", u""};
}

bool RoomMapCatalog::ApplySelection(
    protocol::SessionConfiguration& configuration,
    const protocol::RoomConfigurationRequest& request) {
  // C513 map selection does not authorize a mode or room-kind change.
  if (request.values_at76[7] != configuration.short_at4 ||
      request.values_at76[8] != configuration.values_at20[0]) {
    return false;
  }
  if (request.map_key == u"Random") {
    // Original UI retains the previous difficulty and may send all three
    // strings as Random. A real directory is not a random selector.
    if (!request.map_directory.empty() && request.map_directory != u"Random") {
      return false;
    }
    DefaultRandom(configuration);
    return true;
  }
  const auto* map = Find(request.map_key);
  if (!map || request.map_directory != map->directory ||
      request.values_at76[6] != map->difficulty) {
    return false;
  }
  configuration.texts_at44 = {std::u16string(map->key),
                              std::u16string(map->directory),
                              std::u16string(map->name), u""};
  configuration.value_at140 = map->difficulty;
  return true;
}

const RoomMapDefinition* RoomMapCatalog::Match(
    const protocol::StartGameRequest& request) {
  if (request.variant != 0 || request.flags != 0) {
    return nullptr;
  }
  const auto maps = Maps();
  const auto found =
      std::find_if(maps.begin(), maps.end(), [&](const auto& map) {
        return request.map_resource == map.directory &&
               request.map_attribute == map.name &&
               (request.map_key == map.key || request.map_key == u"Random") &&
               request.value_at76 == map.difficulty &&
               request.rules == map.rules;
      });
  return found == maps.end() ? nullptr : &*found;
}
}  // namespace bubble_fighter::server::features::rooms
