#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_BATTLE_RECORDS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_BATTLE_RECORDS_H_

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "protocol/characters/fighter_info.h"

namespace bubble_fighter::server::features::characters {
// Explicitly identified UI counters only. Storage indices are versioned and
// independent of both native object offsets and network field order.
struct BattleRecords {
  static constexpr std::size_t kCount = 43;
  static constexpr std::size_t kStorageBytes = 4 + 4 * kCount;
  static constexpr std::uint32_t kMaxValue = 0x7FFFFFFF;
  std::array<std::uint32_t, kCount> values{};
  bool IsValid() const noexcept;
  void Bind(protocol::FighterInfo& fighter) const;
  // 1 = result/rate inputs, 2 = disconnects. Other data is retained.
  // Scope is project policy documented in battle_records.md, not a recovered
  // original-server reset algorithm. Returns false when already zero.
  bool Reset(std::uint32_t kind);
  std::vector<std::uint8_t> Encode() const;
  static BattleRecords Decode(std::span<const std::uint8_t> bytes);
  bool operator==(const BattleRecords&) const = default;
};
}  // namespace bubble_fighter::server::features::characters

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_BATTLE_RECORDS_H_
