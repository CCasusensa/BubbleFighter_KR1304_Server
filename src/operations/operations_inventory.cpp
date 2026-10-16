#include "operations/operations_internal.h"

#include <algorithm>
#include <array>
#include "features/inventory/inventory_grant.h"
#include "features/inventory/item_catalog.h"
#include "persistence/inventory/inventory_rows.h"

namespace bubble_fighter::server::operations {
namespace inv = features::inventory;
namespace rows = persistence::inventory_sql;
namespace {
features::characters::CharacterRecord Character(MYSQL* db, std::uint64_t account) {
  const auto result = Query(db, "SELECT CharacterId,HEX(NameUtf16) Name,CharacterType,"
      "ColorIndex,Level,Experience,Lucci,Cash FROM Characters WHERE AccountId=? FOR UPDATE", {account});
  if (result.empty()) throw std::invalid_argument("Account has no character.");
  const auto& row = result[0];
  features::characters::CharacterRecord character;
  character.character_id = row["CharacterId"];
  character.account_id = account;
  character.character_name = Utf16(NameText(row["Name"].get<std::string>()));
  character.character_type = row["CharacterType"];
  character.color_index = row["ColorIndex"];
  character.statistics = {row["Level"], row["Experience"], row["Lucci"], row["Cash"]};
  if (!rows::LockOwner(db, account, character.character_id))
    throw std::invalid_argument("Character is unavailable.");
  return character;
}
constexpr std::array<const char*, 7> fields{"runs", "wins", "losses", "draws", "bubble", "pang", "help"};
std::uint32_t Read32(std::string_view bytes, std::size_t index) {
  std::uint32_t value = 0;
  for (unsigned int shift = 0; shift < 32; shift += 8)
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index++])) << shift;
  return value;
}
}  // namespace

Json Statistics(std::string_view bytes) {
  if (bytes.size() != 176 || Read32(bytes, 0) != 1)
    throw std::runtime_error("Invalid battle records.");
  Json result = Json::object();
  std::uint64_t games = 0;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto value = Read32(bytes, 4 + index * 4);
    if (value > 0x7fffffff) throw std::runtime_error("Invalid battle counter.");
    result[fields[index]] = value;
    if (index < 4) games += value;
  }
  result["games"] = games;
  return result;
}
void OperationsService::Impl::Stats(MYSQL* db, std::uint64_t account, const Json& input) {
  const auto found = Query(db, "SELECT HEX(BattleRecords) bytes FROM Characters WHERE AccountId=? FOR UPDATE", {account});
  if (found.empty()) throw std::invalid_argument("Account has no character.");
  auto bytes = Unhex(found[0]["bytes"].get<std::string>());
  auto stats = Statistics(bytes);
  const auto mode = input.value("mode", "set");
  if (mode != "set" && mode != "add") throw std::invalid_argument("Unsupported statistics mode.");
  const auto values = input.value("stats", input.value("fields", input));
  if (values.contains("games")) throw std::invalid_argument("Games is computed from outcomes.");
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (!values.contains(fields[i])) continue;
    const auto amount = Bounded(values, fields[i]);
    const std::uint64_t value = mode == "add" ? stats[fields[i]].get<std::uint64_t>() + amount : amount;
    if (value > 0x7fffffff) throw std::invalid_argument("Statistics overflow.");
    for (unsigned int b = 0; b < 4; ++b) bytes[4 + i * 4 + b] = static_cast<char>(value >> (8 * b));
  }
  Execute(db, "UPDATE Characters SET BattleRecords=? WHERE AccountId=? AND BattleRecords=UNHEX(?)",
      {SqlBinary(bytes), account, found[0]["bytes"]});
  Kick(account);
}
void OperationsService::Impl::Grant(MYSQL* db, std::uint64_t account, const Json& items) {
  if (!items.is_array() || items.size() > 1024) throw std::invalid_argument("Invalid grant list.");
  if (items.empty()) return;
  auto character = Character(db, account);
  auto inventory = rows::LoadSlots(db, account, character.character_id);
  for (const auto& item : items) {
    if (!item.is_array() || item.size() != 3) throw std::invalid_argument("Expected [item,quantity,days].");
    const auto id = item[0].get<std::uint32_t>();
    const auto* definition = inv::ItemCatalog::Find(id);
    if (!definition) throw std::invalid_argument("Unknown KR item.");
    const auto duration = definition->usage == inv::InventoryUsage::kDuration;
    const auto raw = item[duration ? 2 : 1].get<std::int64_t>();
    if (raw < 1 || raw > (duration ? 36500 : 0x7fffffff))
      throw std::invalid_argument("Grant amount outside KR limits.");
    const auto amount = duration && raw == 36500 ? 0xffffffffu : static_cast<std::uint32_t>(raw);
    const auto status = inv::InventoryGrant::Apply(inventory, id, amount,
        duration ? 10000 : 0x7fffffff, Now());
    if (status == inv::InventoryGrantStatus::kAlreadyOwned) continue;
    if (status != inv::InventoryGrantStatus::kGranted) throw std::invalid_argument("Inventory limit exceeded.");
    auto& slot = *std::find_if(inventory.slots.begin(), inventory.slots.end(),
        [id](const auto& entry) { return entry.item_id == id; });
    rows::PersistGrant(db, character, slot);
  }
  (void)inventory.ToRecords(account, Now());
  Kick(account);
}
void OperationsService::Impl::Bag(MYSQL* db, std::uint64_t account, std::uint32_t item,
                                  const Json& change) {
  auto character = Character(db, account);
  auto inventory = rows::LoadSlots(db, account, character.character_id);
  auto found = std::find_if(inventory.slots.begin(), inventory.slots.end(),
      [item](const auto& entry) { return entry.item_id == item; });
  if (found == inventory.slots.end()) throw std::invalid_argument("Item is not owned.");
  if (change.contains("revision") && change["revision"] != found->revision)
    throw std::invalid_argument("Inventory changed; refresh and retry.");
  if (change.value("remove", false)) {
    auto presets = rows::LoadPresets(db, inventory);
    for (auto& id : presets.primary_items) if (id == item) id = 0;
    for (auto& id : presets.grenade_items) if (id == item) id = 0;
    for (auto& id : presets.crystal_items) if (id == item) id = 0;
    rows::PersistPresets(db, character.character_id, presets);
    if (Execute(db, "DELETE FROM InventorySlots WHERE CharacterId=? AND ItemId=? AND Revision=?",
        {character.character_id, item, found->revision}) != 1)
      throw std::runtime_error("Inventory revision conflict.");
  } else {
    if (found->usage == inv::InventoryUsage::kDuration) {
      const auto days = Bounded(change, "duration_days", 0, 1);
      if (days > 10000 && days != 36500) throw std::invalid_argument("Invalid duration.");
      found->expires_at_unix_seconds = days == 36500 ? std::nullopt :
          std::optional<std::uint64_t>(Now() + days * 86400ULL);
    } else found->quantity = Bounded(change, "quantity", 0, 1);
    rows::PersistGrant(db, character, *found);
  }
  Kick(account);
}
}  // namespace bubble_fighter::server::operations
