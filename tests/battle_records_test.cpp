#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
#include <stdexcept>

#include "features/characters/character_profile.h"
#include "features/inventory/item_use.h"

namespace bf = bubble_fighter::server;
namespace chars = bf::features::characters;
namespace inv = bf::features::inventory;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Battle records check line " +
                             std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool refused = false;
  try {
    action();
  } catch (const std::exception&) {
    refused = true;
  }
  Check(refused);
}
struct Case {
  unsigned offset, kind, value;
};
std::vector<Case> Cases(const std::filesystem::path& root) {
  std::ifstream input(root / "battle_records_cases.txt");
  Check(input.good());
  std::vector<Case> result;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    Case item{};
    Check(static_cast<bool>(row >> item.offset >> item.kind >> item.value));
    result.push_back(item);
  }
  Check(result.size() == 43);
  return result;
}
void Set(proto::FighterInfo& fighter, unsigned offset, unsigned value) {
  if (offset == 40)
    fighter.values_at32[2] = value;
  else if (offset < 300)
    fighter.values_at80.at((offset - 80) / 4) = value;
  else if (offset < 440)
    fighter.values_at404.at((offset - 404) / 4) = value;
  else
    fighter.values_at464.at((offset - 464) / 4) = value;
}
inv::InventorySlot Card(unsigned id, unsigned quantity = 2) {
  inv::InventorySlot row;
  row.item_id = id;
  row.quantity = quantity;
  row.usage = inv::InventoryUsage::kCount;
  row.revision = 7;
  row.text_at36 = u"Retained";
  return row;
}
}  // namespace

int RunBattleRecordsTests(const std::filesystem::path& root) {
  const auto cases = Cases(root);
  chars::CharacterRecord owner{8, 9, u"Fighter", 10, 7, {12, 330, 456, 789}};
  for (std::size_t i = 0; i < cases.size(); ++i)
    owner.battle.values[i] = cases[i].value;
  proto::UserStatNotification envelope;
  envelope.values.fill(777);
  envelope.fighter.values_at32.fill(888);
  envelope.fighter.values_at80.fill(999);
  envelope.fighter.values_at404.fill(999);
  envelope.fighter.values_at464.fill(999);
  envelope.fighter.values_at444.fill(999);
  envelope.fighter.area32_ranks = {{9, {6, 7}}};
  envelope.fighter.zombie_escape_ranks = {{3, {1, 2}}};
  envelope.fighter.social.values.fill(123);
  auto expected = envelope;
  expected.values[0] = 7;
  expected.values[1] = 330;
  expected.values[4] = 456;
  expected.values[5] = 789;
  expected.fighter.values_at32[1] = 12;
  for (const auto& c : cases) Set(expected.fighter, c.offset, c.value);
  auto actual = chars::CharacterProfile::BindKnownFields(envelope, owner, 9);
  Check(actual == expected);
  Check(proto::UserStatNotification::Decode(actual.Encode()) == expected);
  std::vector<std::uint8_t> storage{1, 0, 0, 0};
  for (const auto& c : cases)
    for (unsigned shift = 0; shift < 32; shift += 8)
      storage.push_back(static_cast<std::uint8_t>(c.value >> shift));
  Check(owner.battle.Encode() == storage);
  Check(chars::BattleRecords::Decode(storage) == owner.battle);
  for (std::size_t size = 0; size < storage.size(); ++size)
    Rejects(
        [&] { chars::BattleRecords::Decode(std::span(storage).first(size)); });
  auto extra = storage;
  extra.push_back(0);
  Rejects([&] { chars::BattleRecords::Decode(extra); });
  for (unsigned version : {0u, 2u, 255u}) {
    auto bad = storage;
    bad[0] = static_cast<std::uint8_t>(version);
    Rejects([&] { chars::BattleRecords::Decode(bad); });
  }
  for (std::size_t i = 0; i < 43; ++i) {
    auto bad = owner;
    bad.battle.values[i] = 0x80000000;
    Check(!bad.battle.IsValid());
    Rejects([&] { bad.battle.Encode(); });
    Rejects(
        [&] { chars::CharacterProfile::BindKnownFields(envelope, bad, 9); });
    auto bytes = storage;
    bytes[7 + i * 4] = 0x80;
    Rejects([&] { chars::BattleRecords::Decode(bytes); });
  }
  for (unsigned kind : {1u, 2u}) {
    const unsigned id = 100089 + kind;
    inv::CharacterInventory inventory{9, 8, {Card(id)}};
    auto result = inv::ItemUse::PrepareStatReset(owner, inventory, {id, 1, {}},
                                                 1800000000);
    Check(result.response.result == 0 && result.response.arguments.empty());
    Check(result.packets.size() == 3 && result.packets[0].message_id == 105 &&
          result.packets[1].message_id == 813 &&
          result.packets[2].message_id == 802);
    for (std::size_t i = 0; i < 43; ++i)
      Check(result.character.battle.values[i] ==
            (cases[i].kind == kind ? 0 : cases[i].value));
    Check(result.character.statistics == owner.statistics &&
          result.character.character_name == owner.character_name);
    Check(result.inventory.slots[0].quantity == 1 &&
          result.inventory.slots[0].text_at36 == u"Retained");
    auto notified = proto::UserStatNotification::Decode(result.packets[0]);
    auto profile = chars::CreateCharacterProfileBaseline();
    profile.values[0] = 7;
    profile.values[1] = 330;
    profile.values[4] = 456;
    profile.values[5] = 789;
    profile.fighter.values_at32[1] = 12;
    for (const auto& c : cases)
      Set(profile.fighter, c.offset, c.kind == kind ? 0 : c.value);
    Check(notified == profile);
    auto last_card = inventory;
    last_card.slots[0].quantity = 1;
    auto last = inv::ItemUse::PrepareStatReset(owner, last_card, {id, 1, {}},
                                               1800000000);
    Check(last.response.result == 0 && last.inventory.slots[0].quantity == 0);
    Rejects([&] {
      inv::ItemUse::PrepareStatReset(last.character, last.inventory,
                                     {id, 1, {}}, 1800000000);
    });
    auto again = inv::ItemUse::PrepareStatReset(
        result.character, result.inventory, {id, 1, {}}, 1800000000);
    Check(again.response.result == 9 && again.packets.size() == 1 &&
          again.inventory.slots == result.inventory.slots);
    for (std::size_t i = 0; i < 43; ++i) {
      auto one = owner;
      one.battle = {};
      one.battle.values[i] = 1;
      auto once = inv::ItemUse::PrepareStatReset(one, inventory, {id, 1, {}},
                                                 1800000000);
      Check(once.response.result == (cases[i].kind == kind ? 0u : 9u));
      Check(once.inventory.slots[0].quantity ==
            (cases[i].kind == kind ? 1u : 2u));
    }
    for (int fault = 0; fault < 11; ++fault) {
      auto who = owner;
      auto bag = inventory;
      proto::ItemUseRequest request{id, 1, {}};
      if (fault == 0) ++bag.account_id;
      if (fault == 1) ++bag.character_id;
      if (fault == 2) bag.slots.clear();
      if (fault == 3) bag.slots[0].quantity = 0;
      if (fault == 4) bag.slots[0].equipped = true;
      if (fault == 5) bag.slots[0].revision = UINT64_MAX;
      if (fault == 6) request.value_at4 = 2;
      if (fault == 7) request.arguments = {u"unexpected"};
      if (fault == 8) request.item_id = 100104;
      if (fault == 9) who.statistics.level = 0;
      if (fault == 10) bag.slots[0].usage = inv::InventoryUsage::kDuration;
      Rejects([&] {
        inv::ItemUse::PrepareStatReset(who, bag, request, 1800000000);
      });
    }
  }
  auto invalid = owner.battle;
  Rejects([&] { invalid.Reset(0); });
  Rejects([&] { invalid.Reset(3); });
  std::cout << "Battle records: " << checks << " checks passed\n";
  return checks;
}
