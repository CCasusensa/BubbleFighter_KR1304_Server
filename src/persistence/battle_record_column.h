#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_BATTLE_RECORD_COLUMN_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_BATTLE_RECORD_COLUMN_H_

#include <array>

#include "features/characters/battle_records.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::persistence {
// Keep this object alive and unmoved until the prepared statement has fetched.
struct BattleRecordColumn {
  std::array<std::uint8_t, features::characters::BattleRecords::kStorageBytes>
      bytes{};
  unsigned long length = 0;
  MYSQL_BIND Bind() {
    auto binding = my_sql_commands::Bytes(
        bytes.data(), static_cast<unsigned long>(bytes.size()));
    binding.length = &length;
    return binding;
  }
  features::characters::BattleRecords Read() const {
    if (length != bytes.size())
      throw std::runtime_error("Invalid persisted battle records.");
    return features::characters::BattleRecords::Decode(bytes);
  }
};
}  // namespace bubble_fighter::server::persistence

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_BATTLE_RECORD_COLUMN_H_
