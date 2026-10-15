#ifndef BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_MAP_CATALOG_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_MAP_CATALOG_H_
#include <span>
#include <string_view>

#include "protocol/rooms/room_configuration_request.h"
#include "protocol/rooms/start_game_request.h"
#include "protocol/scenes/session_descriptor.h"

namespace bubble_fighter::server::features::rooms {
struct RoomMapDefinition {
  std::u16string_view key, directory, name;
  std::uint32_t difficulty;
  protocol::StartGameRules rules;
};
class RoomMapCatalog {
 public:
  static std::span<const RoomMapDefinition> Maps();
  static const RoomMapDefinition* Find(std::u16string_view key);
  static void DefaultRandom(protocol::SessionConfiguration& configuration);
  static bool ApplySelection(protocol::SessionConfiguration& configuration,
                             const protocol::RoomConfigurationRequest& request);
  static const RoomMapDefinition* Match(
      const protocol::StartGameRequest& request);
};
}  // namespace bubble_fighter::server::features::rooms

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_MAP_CATALOG_H_
