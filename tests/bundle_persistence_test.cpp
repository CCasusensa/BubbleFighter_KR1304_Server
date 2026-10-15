#include <array>
#include <exception>
#include <iostream>
#include <latch>
#include <source_location>
#include <thread>

#include "features/authentication/credential_crypto.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_inventory_store.h"

namespace bubble_fighter::server::tests {
std::size_t RunBundlePersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace my_sql_commands;
  using namespace features;
  std::size_t checks = 0;
  const auto check = [&](bool value, std::source_location at =
                                         std::source_location::current()) {
    ++checks;
    if (!value)
      throw std::runtime_error("Bundle persistence line " +
                               std::to_string(at.line()));
  };
  const auto rejects = [&](auto action) {
    bool rejected = false;
    try {
      action();
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected);
  };
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  const auto password = authentication::CredentialCrypto::HashPassword(
      "Synthetic-bundle-password");
  const std::array names{"bundle_owner",        "bundle_other",
                         "bundle_fail_receipt", "bundle_fail_component",
                         "bundle_fail_grant",   "bundle_fail_bonus",
                         "bundle_fail_state"};
  const std::array labels{u"BundleOwner",     u"BundleOther", u"BundleReceipt",
                          u"BundleComponent", u"BundleGrant", u"BundleBonus",
                          u"BundleState"};
  std::array<characters::CharacterRecord, 7> players;
  for (std::size_t i = 0; i < players.size(); ++i) {
    check(accounts.CreateAccount(names[i], password));
    players[i] = {0, accounts.FindAccount(names[i])->account_id, labels[i], 10,
                  7};
    check(characters.Create(players[i]) ==
          characters::CharacterCreateResult::kCreated);
    MySqlConnection connection(configuration);
    Statement fund(
        connection.Handle(),
        "UPDATE Characters SET Cash = 2000000, Lucci = 100000 WHERE CharacterId = ?");
    std::array parameters{Number(players[i].character_id)};
    fund.Execute(parameters);
    check(fund.AffectedRows() == 1);
  }
  constexpr std::uint64_t now = 1800000000;
  const auto sql = [&](const std::string& command) {
    MySqlConnection connection(configuration);
    Statement statement(connection.Handle(), command);
    statement.Execute({});
  };
  const auto scalar = [&](const std::string& command) {
    MySqlConnection connection(configuration);
    Statement statement(connection.Handle(), command);
    statement.Execute({});
    std::uint64_t value = 0;
    std::array results{Number(value)};
    check(statement.Fetch(results));
    return value;
  };
  const auto owner = [&](std::size_t i) {
    return std::to_string(players[i].character_id);
  };
  const auto current = [&](std::size_t i) {
    return characters.FindByAccount(players[i].account_id).value();
  };
  const auto load = [&](std::size_t i) {
    return MySqlInventoryStore(configuration)
        .LoadSnapshot(players[i].account_id, players[i].character_id, now + 1)
        .value();
  };
  const auto buy = [&](std::size_t i, std::uint32_t id = 2000001) {
    const auto result =
        MySqlInventoryStore(configuration).Purchase(players[i], {id, 0}, now);
    check(result.response.result == 1);
    return result.response.value_at40;
  };
  const auto pickup = [&](std::size_t i, std::uint32_t id) {
    return MySqlInventoryStore(configuration).Pickup(players[i], {id}, now + 1);
  };
  const auto refund = [&](std::size_t i, std::uint32_t id) {
    return MySqlInventoryStore(configuration)
        .Refund(players[i], {{}, id}, now + 1);
  };
  const auto reset = [&] {
    sql("DELETE c FROM CashBundleComponents c JOIN CashBundles b ON b.SlotId=c.SlotId WHERE b.CharacterId=" +
        owner(0));
    sql("DELETE FROM CashBundles WHERE CharacterId=" + owner(0));
    sql("DELETE FROM CashInventorySlots WHERE CharacterId=" + owner(0));
    sql("DELETE FROM CashPurchases WHERE CharacterId=" + owner(0));
    sql("DELETE FROM InventorySlots WHERE CharacterId=" + owner(0));
    sql("UPDATE Characters SET Cash=2000000, Lucci=100000 WHERE CharacterId=" +
        owner(0));
  };
  const auto id = buy(0);
  check(current(0).statistics.cash == 1995500 &&
        current(0).statistics.lucci == 100000);
  auto before = load(0);
  check(before.cash.bundles.size() == 1 && before.cash.slots.empty() &&
        before.inventory.slots.empty());
  check(before.cash.bundles[0].slot_id == id &&
        before.cash.bundles[0].bonus_lucci == 60000 &&
        before.cash.bundles[0].components[0].amount == 100);
  check(pickup(1, id).response.result == 2 &&
        refund(1, id).response.result == 2);
  auto forged = players[0];
  forged.account_id = players[1].account_id;
  rejects([&] {
    MySqlInventoryStore(configuration).Pickup(forged, {id}, now + 1);
  });
  auto claimed = pickup(0, id);
  check(claimed.response.result == 1 && load(0).cash.bundles.empty());
  check(current(0).statistics.lucci == 160000 &&
        current(0).statistics.cash == 1995500);
  check(load(0).inventory.slots.size() == 1 &&
        load(0).inventory.slots[0].item_id == 100081 &&
        load(0).inventory.slots[0].quantity == 100);
  check(scalar("SELECT BundleState FROM CashBundles WHERE SlotId=" +
               std::to_string(id)) == 1);
  check(pickup(0, id).response.result == 2 &&
        refund(0, id).response.result == 2);
  const auto cancel = buy(0, 2000105);
  check(cancel > id && refund(0, cancel).response.result == 1);
  check(current(0).statistics.cash == 1995500 &&
        current(0).statistics.lucci == 160000);
  check(refund(0, cancel).response.result == 2);
  check(scalar("SELECT BundleState FROM CashBundles WHERE SlotId=" +
               std::to_string(cancel)) == 2);
  reset();
  const auto multi = buy(0, 2000105);
  check(load(0).cash.bundles[0].components.size() == 4);
  check(pickup(0, multi).response.result == 1 &&
        load(0).inventory.slots.size() == 4);
  check(scalar("SELECT COUNT(*) FROM InventorySlots WHERE CharacterId=" +
               owner(0) + " AND ExpiresAtUnixSeconds=1800604801") == 3);
  reset();
  const auto lucci = buy(0, 2000107);
  check(lucci == 0 && current(0).statistics.lucci == 95500 &&
        current(0).statistics.cash == 2000000);
  check(load(0).inventory.slots.size() == 8 && load(0).cash.bundles.empty());
  check(scalar("SELECT COUNT(*) FROM CashBundles WHERE CharacterId=" +
               owner(0)) == 0);
  reset();
  const auto ordinary = MySqlInventoryStore(configuration)
                            .Purchase(players[0], {1000139, 0}, now);
  check(ordinary.response.result == 1);
  const auto bundle = buy(0);
  check(bundle != ordinary.response.value_at40 &&
        load(0).cash.slots.size() == 1 && load(0).cash.bundles.size() == 1);
  check(refund(0, bundle).response.result == 1 &&
        refund(0, ordinary.response.value_at40).response.result == 1);
  check(current(0).statistics.cash == 2000000 && load(0).cash.bundles.empty() &&
        load(0).cash.slots.empty());
  // Failures after debit/header/earlier component insert must roll everything
  // back.
  for (const auto i : {2U, 3U}) {
    const auto sequence = scalar(
        "SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId=1");
    rejects([&] {
      MySqlInventoryStore(configuration)
          .Purchase(players[i], {2000105, 0}, now);
    });
    check(current(i).statistics.cash == 2000000 &&
          load(i).cash.bundles.empty());
    check(scalar("SELECT COUNT(*) FROM CashBundles WHERE CharacterId=" +
                 owner(i)) == 0);
    check(
        scalar(
            "SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId=1") ==
        sequence);
  }
  for (const auto i : {4U, 5U, 6U}) {
    const auto failed = buy(i, i == 4 ? 2000105 : 2000001);
    const auto snapshot = load(i);
    const auto balances = current(i).statistics;
    rejects([&] { pickup(i, failed); });
    check(load(i).inventory.slots == snapshot.inventory.slots &&
          load(i).cash == snapshot.cash && current(i).statistics == balances);
    if (i == 6) {
      rejects([&] { refund(i, failed); });
      check(current(i).statistics == balances && load(i).cash == snapshot.cash);
    } else
      check(refund(i, failed).response.result == 1);
  }
  // Missing components are corruption, never a smaller partially claimable set.
  reset();
  const auto incomplete = buy(0, 2000105);
  sql("DELETE FROM CashBundleComponents WHERE SlotId=" +
      std::to_string(incomplete) + " AND ComponentIndex=3");
  rejects([&] { load(0); });
  rejects([&] { pickup(0, incomplete); });
  check(current(0).statistics.cash == 1995600 &&
        current(0).statistics.lucci == 100000);
  reset();
  // Full Lucci balance keeps all components pending; refund remains possible.
  const auto bounded = buy(0);
  sql("UPDATE Characters SET Lucci=2147483647 WHERE CharacterId=" + owner(0));
  check(pickup(0, bounded).response.result == 2 &&
        load(0).inventory.slots.empty() && load(0).cash.bundles.size() == 1);
  check(refund(0, bounded).response.result == 1 &&
        current(0).statistics.lucci == 2147483647U);
  for (int iteration = 0; iteration < 12; ++iteration) {
    reset();
    const auto purchase = buy(0);
    std::latch ready(2), go(1);
    std::array<std::uint32_t, 2> results{};
    std::array<std::exception_ptr, 2> errors{};
    const auto race = [&](std::size_t lane) {
      ready.count_down();
      go.wait();
      try {
        MySqlInventoryStore store(configuration);
        results[lane] =
            iteration % 3 == 0 || (iteration % 3 == 2 && lane == 0)
                ? store.Refund(players[0], {{}, purchase}, now + 1)
                      .response.result
                : store.Pickup(players[0], {purchase}, now + 1).response.result;
      } catch (...) {
        errors[lane] = std::current_exception();
      }
    };
    std::thread first(race, 0), second(race, 1);
    ready.wait();
    go.count_down();
    first.join();
    second.join();
    check(!errors[0] && !errors[1]);
    check((results[0] == 1) != (results[1] == 1));
    check(load(0).cash.bundles.empty());
    const auto state =
        scalar("SELECT BundleState FROM CashBundles WHERE SlotId=" +
               std::to_string(purchase));
    if (state == 1)
      check(current(0).statistics.cash == 1995500 &&
            current(0).statistics.lucci == 160000 &&
            load(0).inventory.slots.size() == 1 &&
            load(0).inventory.slots[0].quantity == 100);
    else
      check(state == 2 && current(0).statistics.cash == 2000000 &&
            current(0).statistics.lucci == 100000 &&
            load(0).inventory.slots.empty());
  }
  std::cout << "Bundle persistence: " << checks
            << " checks passed; 12 real races.\n";
  return checks;
}
}  // namespace bubble_fighter::server::tests
