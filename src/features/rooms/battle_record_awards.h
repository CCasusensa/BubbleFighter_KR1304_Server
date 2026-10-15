#ifndef BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_BATTLE_RECORD_AWARDS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_BATTLE_RECORD_AWARDS_H_

#include <cstddef>

#include "features/rooms/room_match_settlement.h"

namespace bubble_fighter::server::features::rooms {
// Durable request v3 freezes ordinary mode-1 completion awards. Earlier
// requests retain their original no-counter behavior, including crash recovery.
// This is project policy, not a recovered original-server award algorithm.
class BattleRecordAwards {
 public:
  static constexpr std::uint32_t kRequestVersion = 3;
  static characters::BattleRecords Apply(
      const RoomMatchRequest& request, std::size_t member_index,
      const characters::BattleRecords& before);
};
}  // namespace bubble_fighter::server::features::rooms

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_BATTLE_RECORD_AWARDS_H_
