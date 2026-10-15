#include "features/characters/battle_records.h"

#include <algorithm>
#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::features::characters {
namespace {
struct Field {
  std::uint32_t offset;
  std::uint32_t reset;
};
// 5D7BD0 and original FighterInfo page labels establish these bindings.
// Do not add unnamed fields or infer unused Star pages from older servers.
constexpr std::array<Field, BattleRecords::kCount>
    kFields{
        {
            {40, 2},  // Team disconnects
            {80, 1},  {84, 1},  {88, 1},
            {92, 0},  {96, 0},  {100, 0},  // W/L/D, bubble/pang/rescue
            {176, 1}, {180, 1}, {184, 0},
            {188, 1}, {192, 0}, {196, 2},  // Zombie
            {228, 1}, {236, 0}, {240, 1},
            {244, 1},  // Survival: first,pang,rate numerator,total
            {248, 1}, {252, 1}, {256, 1},
            {264, 0}, {268, 2},  // Boss: total,W/L,best rank,giveup
            {272, 1}, {276, 1}, {280, 0},
            {284, 0}, {288, 2},  // Bomb: W/L,plant,defuse,escape
            {404, 1}, {408, 1}, {412, 1},
            {416, 0}, {420, 0}, {424, 0},
            {428, 2}, {432, 0},  // Captain
            {464, 1}, {468, 1}, {472, 1},
            {476, 2}, {480, 0}, {484, 0},
            {488, 0}, {492, 0}  // All-pang
        }};
}  // namespace
bool BattleRecords::IsValid() const noexcept {
  return std::all_of(values.begin(), values.end(),
                     [](auto n) { return n <= kMaxValue; });
}
void BattleRecords::Bind(protocol::FighterInfo& fighter) const {
  if (!IsValid())
    throw std::invalid_argument("Invalid persisted battle records.");
  for (std::size_t i = 0; i < kFields.size(); ++i) {
    const auto offset = kFields[i].offset;
    if (offset == 40)
      fighter.values_at32[2] = values[i];
    else if (offset <= 288)
      fighter.values_at80[(offset - 80) / 4] = values[i];
    else if (offset <= 432)
      fighter.values_at404[(offset - 404) / 4] = values[i];
    else
      fighter.values_at464[(offset - 464) / 4] = values[i];
  }
}
bool BattleRecords::Reset(std::uint32_t kind) {
  if (!IsValid() || (kind != 1 && kind != 2))
    throw std::invalid_argument("Invalid battle reset operation.");
  bool changed = false;
  for (std::size_t i = 0; i < kFields.size(); ++i) {
    if (kFields[i].reset == kind) {
      changed = changed || values[i] != 0;
      values[i] = 0;
    }
  }
  return changed;
}
std::vector<std::uint8_t> BattleRecords::Encode() const {
  if (!IsValid())
    throw std::invalid_argument("Invalid persisted battle records.");
  protocol::PacketWriter writer;
  writer.WriteUInt32(1);
  for (const auto value : values) writer.WriteUInt32(value);
  return writer.TakeBytes();
}
BattleRecords BattleRecords::Decode(std::span<const std::uint8_t> bytes) {
  if (bytes.size() != kStorageBytes)
    throw std::invalid_argument("Invalid persisted battle records.");
  protocol::PacketReader reader(bytes);
  if (reader.ReadUInt32() != 1)
    throw std::invalid_argument("Invalid persisted battle records.");
  BattleRecords result;
  for (auto& value : result.values) value = reader.ReadUInt32();
  if (!result.IsValid())
    throw std::invalid_argument("Invalid persisted battle records.");
  return result;
}
}  // namespace bubble_fighter::server::features::characters
