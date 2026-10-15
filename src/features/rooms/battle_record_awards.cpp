#include "features/rooms/battle_record_awards.h"

#include <algorithm>
#include <stdexcept>

namespace bubble_fighter::server::features::rooms {
characters::BattleRecords BattleRecordAwards::Apply(
    const RoomMatchRequest& request, std::size_t member_index,
    const characters::BattleRecords& before) {
  request.Validate();
  if (!before.IsValid())
    throw std::invalid_argument("Invalid persisted battle records.");
  const auto& member = request.members.at(member_index);
  auto after = before;
  if (request.format_version != kRequestVersion) return after;
  const auto increment = [&](std::size_t index, std::uint32_t amount) {
    auto& value = after.values[index];
    value += std::min(amount, characters::BattleRecords::kMaxValue - value);
  };
  // Storage v1 indexes1..6 bind FighterInfo+80/+84/+88/+92/+96/+100.
  // Scores are frozen accepted transitions, never C617 client result claims.
  const auto winner = request.Winner();
  increment(winner == 0 ? 3 : (member.team == winner ? 1 : 2), 1);
  for (std::size_t i = 0; i < 3; ++i) increment(4 + i, member.scores[i]);
  // Deaths do not imply departure. Other modes and escape counts are retained.
  return after;
}
}  // namespace bubble_fighter::server::features::rooms
