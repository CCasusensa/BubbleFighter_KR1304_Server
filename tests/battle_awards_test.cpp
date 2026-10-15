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
#include "features/rooms/battle_record_awards.h"
#include "persistence/inventory/inventory_rows.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_inventory_store.h"
#include "persistence/my_sql_match_store.h"

namespace bf = bubble_fighter::server;
namespace rooms = bf::features::rooms;
namespace chars = bf::features::characters;
namespace db = bf::persistence;
namespace sql = db::my_sql_commands;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Battle awards check line " +
                             std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected);
}
rooms::RoomMatchRequest Request(unsigned version = 3) {
  rooms::RoomMatchRequest r;
  r.format_version = version;
  r.match_id = rooms::CreateRoomMatchId();
  r.duration_milliseconds = 240000;
  r.team_scores = {9, 4};
  r.members = {{0, 11, 21, 1, {3, 5, 7, 11}}, {1, 12, 22, 2, {2, 4, 6, 8}}};
  return r;
}
}  // namespace

int RunBattleAwardsTests(const std::filesystem::path& directory) {
  std::ifstream file(directory / "battle_awards_cases.txt");
  Check(file.good());
  std::string line;
  unsigned rows = 0;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream input(line);
    auto r = Request();
    unsigned team = 0, base = 0;
    std::array<unsigned, 6> expected{};
    Check(static_cast<bool>(
        input >> r.format_version >> team >> r.team_scores[0] >>
        r.team_scores[1] >> r.members[0].scores[0] >> r.members[0].scores[1] >>
        r.members[0].scores[2] >> r.members[0].scores[3] >> base));
    for (auto& n : expected) Check(static_cast<bool>(input >> n));
    r.members[0].team = static_cast<std::uint8_t>(team);
    chars::BattleRecords before;
    before.values.fill(base);
    const auto result = rooms::BattleRecordAwards::Apply(r, 0, before);
    for (std::size_t i = 0; i < 43; ++i)
      Check(result.values[i] == (i >= 1 && i <= 6 ? expected[i - 1] : base));
    Check(before.values[1] == base);
    auto bytes = r.Encode();
    Check(rooms::RoomMatchRequest::Decode(bytes).Encode() == bytes);
    Check(bytes[0] == r.format_version && bytes[1] == 0);
    if (r.format_version == 3) {
      auto old = r;
      old.format_version = 2;
      auto old_bytes = old.Encode();
      old_bytes[0] = 3;
      Check(bytes == old_bytes);
      for (std::size_t n = 0; n < bytes.size(); ++n)
        Rejects([&] {
          rooms::RoomMatchRequest::Decode(std::span(bytes).first(n));
        });
      bytes.push_back(0);
      Rejects([&] { rooms::RoomMatchRequest::Decode(bytes); });
    }
    ++rows;
  }
  Check(rows == 12);
  auto r = Request();
  for (auto version : {0u, 4u, 255u}) {
    r.format_version = version;
    Rejects([&] { rooms::BattleRecordAwards::Apply(r, 0, {}); });
  }
  r = Request();
  Rejects([&] { rooms::BattleRecordAwards::Apply(r, 2, {}); });
  chars::BattleRecords bad;
  bad.values[42] = 0x80000000;
  Rejects([&] { rooms::BattleRecordAwards::Apply(r, 0, bad); });
  std::cout << "Battle awards policy: " << checks << " checks\n";
  return checks;
}

int RunBattleAwardsPersistenceTests(
    const std::filesystem::path& configuration) {
  const auto execute = [&](const std::string& command) {
    db::MySqlConnection c(configuration);
    sql::Statement q(c.Handle(), command);
    q.Execute({});
  };
  const auto scalar = [&](const std::string& command) {
    db::MySqlConnection c(configuration);
    sql::Statement q(c.Handle(), command);
    q.Execute({});
    std::uint64_t value = 0;
    std::array out{sql::Number(value)};
    Check(q.Fetch(out));
    return value;
  };
  Check(scalar("SELECT DATABASE() REGEXP '^BfAwardsTests_[0-9a-f]{12}$'") == 1);
  db::MySqlAccountStore accounts(configuration);
  db::MySqlCharacterStore characters(configuration);
  auto password = bf::features::authentication::CredentialCrypto::HashPassword(
      "Synthetic-awards-password");
  std::array<chars::CharacterRecord, 2> players;
  for (std::size_t i = 0; i < 2; ++i) {
    const auto name = "awards_" + std::to_string(i);
    Check(accounts.CreateAccount(name, password));
    players[i] = {0, accounts.FindAccount(name)->account_id, u"Awards", 10, 7};
    players[i].character_name += static_cast<char16_t>(u'0' + i);
    Check(characters.Create(players[i]) ==
          chars::CharacterCreateResult::kCreated);
  }
  const auto id = [&](std::size_t i) {
    return std::to_string(players[i].character_id);
  };
  const auto fresh = [&](std::size_t i) {
    return *db::MySqlCharacterStore(configuration)
                .FindByAccount(players[i].account_id);
  };
  const auto seed = [&](unsigned base, bool card = true) {
    for (auto who : players) {
      db::MySqlConnection c(configuration);
      sql::Transaction t(c.Handle());
      Check(db::inventory_sql::LockOwner(c.Handle(), who.account_id,
                                         who.character_id));
      who.battle.values.fill(base);
      auto bytes = who.battle.Encode();
      {
        sql::Statement q(
            c.Handle(),
            "UPDATE Characters SET Level=1,Experience=0,Lucci=0,Cash=0,BattleRecords=? WHERE CharacterId=?");
        std::array p{
            sql::Bytes(bytes.data(), static_cast<unsigned long>(bytes.size())),
            sql::Number(who.character_id)};
        q.Execute(p);
      }
      if (card) {
        auto inventory = db::inventory_sql::LoadSlots(
            c.Handle(), who.account_id, who.character_id);
        auto at =
            std::find_if(inventory.slots.begin(), inventory.slots.end(),
                         [](const auto& s) { return s.item_id == 100090; });
        bf::features::inventory::InventorySlot slot;
        if (at != inventory.slots.end()) slot = *at;
        slot.item_id = 100090;
        slot.quantity = 2;
        slot.usage = bf::features::inventory::InventoryUsage::kCount;
        db::inventory_sql::PersistGrant(c.Handle(), who, slot);
      }
      t.Commit();
    }
  };
  const auto request = [&](unsigned version = 3) {
    auto r = Request(version);
    for (std::size_t i = 0; i < 2; ++i) {
      r.members[i].account_id = players[i].account_id;
      r.members[i].character_id = players[i].character_id;
    }
    return r;
  };
  const auto settle = [&](const rooms::RoomMatchRequest& r) {
    return db::MySqlMatchStore(configuration).Settle(r);
  };
  const auto use = [&] {
    return db::MySqlInventoryStore(configuration)
        .UseItem(players[0], {100090, 1, {}}, 1800000000);
  };
  const auto quantity = [&] {
    return scalar("SELECT Quantity FROM InventorySlots WHERE CharacterId=" +
                  id(0) + " AND ItemId=100090");
  };
  const auto matches = [&] {
    return scalar(
        "SELECT COUNT(*) FROM MatchSettlements WHERE OCTET_LENGTH(ResultData)>0");
  };
  // Older pending requests retain their no-counter semantics after an upgrade.
  for (unsigned version : {1u, 2u}) {
    seed(10);
    auto r = request(version);
    db::MySqlMatchStore(configuration).SavePending(r);
    auto result = settle(r);
    for (std::size_t i = 0; i < 2; ++i) {
      Check(fresh(i).battle.values[1] == 10);
      Check(fresh(i).battle.values[4] == 10);
    }
    Check(settle(r).Encode() == result.Encode());
  }
  seed(10);
  auto r = request();
  auto result = settle(r);
  auto receipt = result.Encode();
  Check(receipt[0] == 1);
  Check(fresh(0).battle.values[1] == 11 && fresh(1).battle.values[2] == 11);
  Check(fresh(0).battle.values[4] == 13 && fresh(1).battle.values[6] == 16);
  for (std::size_t i = 0; i < 2; ++i) {
    auto b = fresh(i).battle;
    for (std::size_t j = 0; j < 43; ++j)
      if (j == 0 || j > 6) Check(b.values[j] == 10);
  }
  const auto settled_stats = fresh(0).statistics;
  Check(use().response.result == 0 && quantity() == 1);
  auto reset = fresh(0).battle;
  Check(reset.values[1] == 0 && reset.values[4] == 13);
  execute("UPDATE Characters SET Cash=23 WHERE CharacterId=" + id(0));
  for (unsigned n = 0; n < 3; ++n) Check(settle(r).Encode() == receipt);
  Check(fresh(0).battle == reset && fresh(0).statistics.cash == 23 &&
        fresh(0).statistics.experience == settled_stats.experience);
  auto changed = r;
  changed.format_version = 2;
  Rejects([&] { settle(changed); });
  changed = r;
  ++changed.members[0].scores[0];
  Rejects([&] { settle(changed); });
  Check(fresh(0).battle == reset);
  // Receipt insertion fails after both counter/wallet writes. They all roll
  // back, leaving the same durable pending request recoverable on a fresh
  // connection.
  seed(10);
  r = request();
  execute("UPDATE Characters SET Cash=99 WHERE CharacterId=" + id(1));
  const auto before_count = matches();
  Rejects([&] { settle(r); });
  Check(matches() == before_count);
  for (std::size_t i = 0; i < 2; ++i)
    Check(fresh(i).battle.values[1] == 10 && fresh(i).battle.values[4] == 10 &&
          fresh(i).statistics.experience == 0);
  auto pending = db::MySqlMatchStore(configuration).LoadPending({});
  Check(pending.size() == 1 && pending[0].Encode() == r.Encode());
  execute("UPDATE Characters SET Cash=0 WHERE CharacterId=" + id(1));
  result = settle(pending[0]);
  Check(matches() == before_count + 1);
  Check(fresh(0).battle.values[1] == 11 && fresh(1).battle.values[2] == 11);
  Check(db::MySqlMatchStore(configuration).LoadPending({}).empty());
  // Counter-only changes must commit when monetary rewards are all zero.
  seed(0);
  r = request();
  r.rules.completion_experience = r.rules.completion_lucci =
      r.rules.win_experience = r.rules.win_lucci = r.rules.pang_experience =
          r.rules.pang_lucci = r.rules.rescue_experience =
              r.rules.rescue_lucci = 0;
  settle(r);
  Check(fresh(0).statistics.experience == 0 && fresh(0).battle.values[1] == 1);
  seed(chars::BattleRecords::kMaxValue);
  r = request();
  settle(r);
  Check(fresh(0).battle.values[1] == chars::BattleRecords::kMaxValue &&
        fresh(0).battle.values[4] == chars::BattleRecords::kMaxValue);
  // Even a completely saturated/no-money match commits an immutable receipt.
  r = request();
  r.rules.completion_experience = r.rules.completion_lucci =
      r.rules.win_experience = r.rules.win_lucci = r.rules.pang_experience =
          r.rules.pang_lucci = r.rules.rescue_experience =
              r.rules.rescue_lucci = 0;
  result = settle(r);
  Check(settle(r).Encode() == result.Encode());
  unsigned races = 0;
  for (unsigned trial = 0; trial < 30; ++trial) {
    seed(10);
    auto one = request();
    auto two = trial < 12 ? one : request();
    std::array<std::exception_ptr, 2> errors{};
    std::array<std::vector<std::uint8_t>, 2> receipts;
    std::latch ready(2), go(1);
    std::array<std::thread, 2> workers;
    for (std::size_t i = 0; i < 2; ++i)
      workers[i] = std::thread([&, i] {
        ready.count_down();
        go.wait();
        try {
          if (trial >= 18 && i == 1) {
            if (use().response.result != 0)
              throw std::runtime_error("Reset race rejected");
          } else
            receipts[i] = settle(i == 0 ? one : two).Encode();
        } catch (...) {
          errors[i] = std::current_exception();
        }
      });
    ready.wait();
    go.count_down();
    for (auto& worker : workers) worker.join();
    for (const auto& error : errors) {
      if (error) {
        try {
          std::rethrow_exception(error);
        } catch (const std::exception& e) {
          throw std::runtime_error("Race " + std::to_string(trial) + ": " +
                                   e.what());
        }
      }
      Check(!error);
    }
    const auto b = fresh(0).battle;
    if (trial < 12) {
      Check(receipts[0] == receipts[1]);
      Check(b.values[1] == 11 && b.values[4] == 13);
    } else if (trial < 18) {
      Check(b.values[1] == 12 && b.values[4] == 16);
    } else {
      Check(b.values[1] <= 1 && b.values[2] == 0 && b.values[3] == 0);
      Check(b.values[4] == 13 && b.values[5] == 15 && b.values[6] == 17);
      Check(quantity() == 1);
    }
    Check(b.values[0] == 10 && b.values[9] == 10);
    const auto saved = b;
    Check(settle(one).Encode() == receipts[0]);
    Check(fresh(0).battle == saved);
    ++races;
  }
  seed(0);
  r = request();
  r.members[0].account_id = players[1].account_id;
  Rejects([&] { settle(r); });
  Check(fresh(0).battle == chars::BattleRecords{});
  r = request();
  execute("UPDATE Accounts SET IsDisabled=1 WHERE AccountId=" +
          std::to_string(players[1].account_id));
  Rejects([&] { settle(r); });
  Check(fresh(0).battle == chars::BattleRecords{});
  execute("UPDATE Accounts SET IsDisabled=0 WHERE AccountId=" +
          std::to_string(players[1].account_id));
  seed(0);  // Natural network tests begin with valid cards and no recorded
            // matches.
  std::cout << "Battle awards MySQL: " << checks << " checks, " << races
            << " races\n";
  return checks;
}
