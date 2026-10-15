#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <source_location>
#include <sstream>
#include <thread>

#include "features/authentication/credential_crypto.h"
#include "features/characters/character_profile.h"
#include "features/inventory/item_use.h"
#include "persistence/gifts/gift_rows.h"
#include "persistence/inventory/inventory_rows.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_inventory_store.h"
#include "persistence/my_sql_match_store.h"

namespace bf = bubble_fighter::server;
namespace chars = bf::features::characters;
namespace inv = bf::features::inventory;
namespace proto = bf::protocol;
namespace db = bf::persistence;
namespace sql = db::my_sql_commands;
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

int RunBattleRecordsPersistenceTests(
    const std::filesystem::path& configuration) {
  const auto execute = [&](const std::string& command) {
    db::MySqlConnection connection(configuration);
    sql::Statement s(connection.Handle(), command);
    s.Execute({});
  };
  const auto scalar = [&](const std::string& command) {
    db::MySqlConnection connection(configuration);
    sql::Statement s(connection.Handle(), command);
    s.Execute({});
    std::uint64_t n = 0;
    std::array result{sql::Number(n)};
    Check(s.Fetch(result));
    return n;
  };
  Check(scalar("SELECT DATABASE() REGEXP '^BfBattleTests_[0-9a-f]{12}$'") == 1);
  db::MySqlAccountStore accounts(configuration);
  db::MySqlCharacterStore characters(configuration);
  const auto password =
      bf::features::authentication::CredentialCrypto::HashPassword(
          "Synthetic-battle-password");
  std::array<chars::CharacterRecord, 3> players;
  for (std::size_t i = 0; i < players.size(); ++i) {
    const auto username = "battle_" + std::to_string(i);
    Check(accounts.CreateAccount(username, password));
    players[i] = {0, accounts.FindAccount(username)->account_id, u"Battle", 10,
                  7};
    players[i].character_name += static_cast<char16_t>(u'0' + i);
    players[i].battle.values.fill(777);
    Check(characters.Create(players[i]) ==
          chars::CharacterCreateResult::kCreated);
    Check(players[i].battle == chars::BattleRecords{});
  }
  const auto id = [&](int i) {
    return std::to_string(players[i].character_id);
  };
  const auto seed = [&](int i, unsigned quantity = 2) {
    auto who = players[i];
    for (std::size_t j = 0; j < 43; ++j)
      who.battle.values[j] = static_cast<unsigned>(11 + j);
    db::MySqlConnection connection(configuration);
    sql::Transaction transaction(connection.Handle());
    Check(db::inventory_sql::LockOwner(connection.Handle(), who.account_id,
                                       who.character_id));
    auto bytes = who.battle.Encode();
    sql::Statement update(
        connection.Handle(),
        "UPDATE Characters SET BattleRecords=? WHERE CharacterId=?");
    std::array parameters{
        sql::Bytes(bytes.data(), static_cast<unsigned long>(bytes.size())),
        sql::Number(who.character_id)};
    update.Execute(parameters);
    for (unsigned item : {100090u, 100091u}) {
      auto inventory = db::inventory_sql::LoadSlots(
          connection.Handle(), who.account_id, who.character_id);
      auto found =
          std::find_if(inventory.slots.begin(), inventory.slots.end(),
                       [&](const auto& s) { return s.item_id == item; });
      auto row = found == inventory.slots.end() ? Card(item, quantity) : *found;
      if (found == inventory.slots.end()) row.revision = 0;
      row.quantity = quantity;
      db::inventory_sql::PersistGrant(connection.Handle(), who, row);
    }
    transaction.Commit();
    return who;
  };
  const auto use = [&](int i, unsigned item) {
    return db::MySqlInventoryStore(configuration)
        .UseItem(players[i], {item, 1, {}}, 1800000000);
  };
  const auto fresh = [&](int i) {
    return *db::MySqlCharacterStore(configuration)
                .FindByAccount(players[i].account_id);
  };
  const auto quantity = [&](int i, unsigned item) {
    return scalar("SELECT Quantity FROM InventorySlots WHERE CharacterId=" +
                  id(i) + " AND ItemId=" + std::to_string(item));
  };
  auto original = seed(0);
  seed(1);
  Check(fresh(0).battle == original.battle);
  {
    db::MySqlConnection connection(configuration);
    Check(db::gift_sql::FindRecipient(connection.Handle(),
                                      original.character_name)
              ->battle == original.battle);
  }
  auto first = use(0, 100090);
  Check(first.response.result == 0);
  auto expected = original.battle;
  Check(expected.Reset(1));
  Check(fresh(0).battle == expected && quantity(0, 100090) == 1);
  auto repeat = use(0, 100090);
  Check(repeat.response.result == 9 && quantity(0, 100090) == 1);
  Check(use(0, 100091).response.result == 0);
  expected.Reset(2);
  Check(fresh(0).battle == expected && quantity(0, 100091) == 1);
  Check(fresh(1).battle == original.battle);
  // Real competing connections, held behind a common start barrier.
  for (unsigned trial = 0; trial < 12; ++trial) {
    original = seed(0, 3);
    const unsigned item = trial % 2 ? 100091 : 100090;
    std::array<unsigned, 2> results{};
    std::array<std::exception_ptr, 2> errors;
    std::latch ready(2), go(1);
    std::array<std::thread, 2> workers;
    for (std::size_t j = 0; j < 2; ++j)
      workers[j] = std::thread([&, j] {
        ready.count_down();
        go.wait();
        try {
          results[j] = use(0, item).response.result;
        } catch (...) {
          errors[j] = std::current_exception();
        }
      });
    ready.wait();
    go.count_down();
    for (auto& worker : workers) worker.join();
    for (const auto& error : errors) Check(!error);
    std::sort(results.begin(), results.end());
    Check(results == std::array<unsigned, 2>{0, 9});
    Check(quantity(0, item) == 2);
    auto want = original.battle;
    want.Reset(item - 100089);
    Check(fresh(0).battle == want);
  }
  for (unsigned trial = 0; trial < 6; ++trial) {
    original = seed(0, 3);
    std::array<unsigned, 2> results{};
    std::array<std::exception_ptr, 2> errors;
    std::latch ready(2), go(1);
    std::array<std::thread, 2> workers;
    for (std::size_t j = 0; j < 2; ++j)
      workers[j] = std::thread([&, j] {
        ready.count_down();
        go.wait();
        try {
          results[j] =
              use(0, static_cast<unsigned>(100090 + j)).response.result;
        } catch (...) {
          errors[j] = std::current_exception();
        }
      });
    ready.wait();
    go.count_down();
    for (auto& worker : workers) worker.join();
    for (const auto& error : errors) Check(!error);
    Check(results == std::array<unsigned, 2>{0, 0});
    Check(quantity(0, 100090) == 2 && quantity(0, 100091) == 2);
    auto want = original.battle;
    want.Reset(1);
    want.Reset(2);
    Check(fresh(0).battle == want);
  }
  // A match receipt contains historical wallets, not an authoritative current
  // counter snapshot. Replaying it must never undo a later reset.
  original = seed(0);
  bf::features::rooms::RoomMatchRequest match;
  match.match_id = bf::features::rooms::CreateRoomMatchId();
  match.duration_milliseconds = 240000;
  match.members = {{0, players[0].account_id, players[0].character_id, 1, {}}};
  const auto receipt =
      db::MySqlMatchStore(configuration).Settle(match).Encode();
  Check(fresh(0).battle == original.battle);
  Check(use(0, 100090).response.result == 0);
  const auto after = fresh(0);
  Check(db::MySqlMatchStore(configuration).Settle(match).Encode() == receipt);
  Check(fresh(0) == after);
  auto forged = players[0];
  forged.account_id = players[1].account_id;
  Rejects([&] {
    db::MySqlInventoryStore(configuration)
        .UseItem(forged, {100091, 1, {}}, 1800000000);
  });
  forged = players[0];
  forged.character_name = u"Changed";
  Rejects([&] {
    db::MySqlInventoryStore(configuration)
        .UseItem(forged, {100091, 1, {}}, 1800000000);
  });
  execute("UPDATE Accounts SET IsDisabled=1 WHERE AccountId=" +
          std::to_string(players[0].account_id));
  Rejects([&] { use(0, 100091); });
  execute("UPDATE Accounts SET IsDisabled=0 WHERE AccountId=" +
          std::to_string(players[0].account_id));
  Check(fresh(0) == after);
  // The harness trigger fails the inventory write AFTER the counter write.
  original = seed(2);
  Rejects([&] { use(2, 100090); });
  Check(fresh(2).battle == original.battle && quantity(2, 100090) == 2);
  // Unknown version/length rejected by SQL; sign-bit corruption by every
  // reader.
  Rejects([&] {
    execute(
        "UPDATE Characters SET BattleRecords=0x02000000 WHERE CharacterId=" +
        id(1));
  });
  Rejects([&] {
    execute(
        "UPDATE Characters SET BattleRecords=INSERT(BattleRecords,1,1,0x02) WHERE CharacterId=" +
        id(1));
  });
  execute(
      "UPDATE Characters SET BattleRecords=INSERT(BattleRecords,8,1,0x80) WHERE CharacterId=" +
      id(1));
  Rejects([&] { fresh(1); });
  Rejects([&] { use(1, 100090); });
  Check(quantity(1, 100090) == 2);
  seed(1);
  seed(0);  // Independent TCP fixtures start with complete nonzero data.
  std::cout << "Battle records MySQL: " << checks << " checks passed\n";
  return checks;
}
