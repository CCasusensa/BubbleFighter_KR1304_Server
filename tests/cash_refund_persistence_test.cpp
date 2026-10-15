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
std::size_t RunCashRefundPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace my_sql_commands;
  using namespace features;
  std::size_t checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Cash refund persistence line " +
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
      "Synthetic-refund-password");
  std::array<characters::CharacterRecord, 8> players;
  const std::array names{"refund_owner",        "refund_other",
                         "refund_fail_receipt", "refund_fail_delete",
                         "refund_fail_claim",   "refund_fail_credit",
                         "refund_fail_mark",    "refund_fail_part"};
  const std::array labels{u"RefundOwner",  u"RefundOther", u"RefundReceipt",
                          u"RefundDelete", u"RefundClaim", u"RefundCredit",
                          u"RefundMark",   u"RefundPart"};
  for (std::size_t i = 0; i < players.size(); ++i) {
    check(accounts.CreateAccount(names[i], password));
    players[i] = {0, accounts.FindAccount(names[i])->account_id, labels[i], 10,
                  7};
    check(characters.Create(players[i]) ==
          characters::CharacterCreateResult::kCreated);
    MySqlConnection connection(configuration);
    Statement fund(
        connection.Handle(),
        "UPDATE Characters SET Cash = 2000000 WHERE CharacterId = ?");
    std::array values{Number(players[i].character_id)};
    fund.Execute(values);
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
    std::array result{Number(value)};
    check(statement.Fetch(result));
    return value;
  };
  const auto owner = [&](std::size_t i) {
    return std::to_string(players[i].character_id);
  };
  const auto cash = [&](std::size_t i) {
    return characters.FindByAccount(players[i].account_id)->statistics.cash;
  };
  const auto load = [&](std::size_t i) {
    return MySqlInventoryStore(configuration)
        .LoadSnapshot(players[i].account_id, players[i].character_id, now)
        .value();
  };
  const auto buy = [&](std::size_t i, std::uint32_t stock = 1010816) {
    auto result = MySqlInventoryStore(configuration)
                      .Purchase(players[i], {stock, 0}, now);
    check(result.response.result == 1);
    return result.response.value_at40;
  };
  const auto refund = [&](std::size_t i, std::uint32_t slot,
                          std::uint64_t clock = 1800000001) {
    return MySqlInventoryStore(configuration)
        .Refund(players[i], {{}, slot}, clock);
  };
  const auto reset = [&](std::size_t i) {
    sql("DELETE FROM CashInventorySlots WHERE CharacterId = " + owner(i));
    sql("DELETE FROM CashPurchases WHERE CharacterId = " + owner(i));
    sql("DELETE FROM InventorySlots WHERE CharacterId = " + owner(i));
    sql("UPDATE Characters SET Cash = 2000000 WHERE CharacterId = " + owner(i));
  };
  const auto id = buy(0);
  auto snapshot = load(0);
  check(snapshot.cash.slots.size() == 2 && snapshot.cash.purchases.size() == 1);
  check(snapshot.cash.purchases[0].amount == 100000 &&
        snapshot.cash.purchases[0].cash_price == 999999);
  check(snapshot.cash.slots[0].purchase_id == id &&
        snapshot.cash.slots[1].purchase_id == id);
  check(snapshot.cash.CreateNotification(players[0].account_id, now + 1)
            .slots[0]
            .byte_at48 == 1);
  check(cash(0) == 1000001);
  check(refund(1, id).response.result == 2 && cash(1) == 2000000);
  auto forged = players[0];
  forged.account_id = players[1].account_id;
  rejects([&] {
    MySqlInventoryStore(configuration).Refund(forged, {{}, id}, now + 1);
  });
  auto stale = players[0];
  stale.character_name = u"WrongName";
  rejects([&] {
    MySqlInventoryStore(configuration).Refund(stale, {{}, id}, now + 1);
  });
  sql("UPDATE Accounts SET IsDisabled = 1 WHERE AccountId = " +
      std::to_string(players[0].account_id));
  rejects([&] { refund(0, id); });
  sql("UPDATE Accounts SET IsDisabled = 0 WHERE AccountId = " +
      std::to_string(players[0].account_id));
  check(refund(0, id, now - 1).response.result == 2);
  check(refund(0, id, now + 604800).response.result == 4);
  check(load(0).cash == snapshot.cash && cash(0) == 1000001);
  const auto success = refund(0, id + 1, now + 604799);
  check(success.response.result == 1 && success.cash_returned == 999999);
  check(success.removed_slot_ids == std::vector<std::uint32_t>({id, id + 1}));
  check(cash(0) == 2000000 && load(0).cash.slots.empty());
  check(scalar("SELECT PurchaseState FROM CashPurchases WHERE PurchaseId = " +
               std::to_string(id)) == 2);
  check(
      scalar(
          "SELECT RefundedAtUnixSeconds FROM CashPurchases WHERE PurchaseId = " +
          std::to_string(id)) == now + 604799);
  check(scalar("SELECT COUNT(*) FROM CashPurchaseSlots WHERE PurchaseId = " +
               std::to_string(id)) == 0);
  check(refund(0, id).response.result == 2 && cash(0) == 2000000);

  for (const auto part : {0U, 1U}) {
    reset(0);
    const auto purchase = buy(0);
    const auto pickup = MySqlInventoryStore(configuration)
                            .Pickup(players[0], {purchase + part}, now + 1);
    check(pickup.response.result == 1);
    check(load(0).cash.purchases[0].claimed);
    check(load(0)
              .cash.CreateNotification(players[0].account_id, now + 2)
              .slots[0]
              .byte_at48 == 0);
    check(refund(0, purchase + (1 - part)).response.result == 2 &&
          cash(0) == 1000001);
    check(MySqlInventoryStore(configuration)
              .Pickup(players[0], {purchase + (1 - part)}, now + 2)
              .response.result == 1);
    check(load(0).inventory.slots[0].quantity == 100000 &&
          load(0).cash.slots.empty());
  }
  reset(0);
  const auto overflow = buy(0, 1000139);
  sql("UPDATE Characters SET Cash = 2147483647 WHERE CharacterId = " +
      owner(0));
  check(refund(0, overflow).response.result == 2 &&
        !load(0).cash.slots.empty());
  check(cash(0) == 0x7FFFFFFF);
  reset(0);

  // Historical rows remain claimable without inventing a purchase relationship.
  sql("INSERT INTO CashInventorySlots (SlotId,CharacterId,StockId,ItemId,UsageKind,Amount,MaxAmount,CashPrice,PurchasedAtUnixSeconds) VALUES (99999000," +
      owner(0) + ",1000139,205034,1,100,20000,1000,1800000000)");
  check(load(0).cash.purchases.empty());
  check(refund(0, 99999000).response.result == 2);
  check(MySqlInventoryStore(configuration)
            .Pickup(players[0], {99999000}, now + 1)
            .response.result == 1);
  check(cash(0) == 2000000 && load(0).inventory.slots[0].quantity == 100);

  // Synthetic triggers in the owning test schema fail after different writes.
  for (const auto failed_owner : {2U, 7U}) {
    const auto sequence = scalar(
        "SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId = 1");
    rejects([&] {
      MySqlInventoryStore(configuration)
          .Purchase(players[failed_owner], {1010816, 0}, now);
    });
    check(cash(failed_owner) == 2000000 &&
          load(failed_owner).cash.slots.empty());
    check(scalar("SELECT COUNT(*) FROM CashPurchases WHERE CharacterId = " +
                 owner(failed_owner)) == 0);
    check(
        scalar(
            "SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId = 1") ==
        sequence);
  }
  for (const auto i : {3U, 5U, 6U}) {
    const auto purchase = buy(i);
    const auto before = load(i).cash;
    rejects([&] { refund(i, purchase); });
    check(cash(i) == 1000001 && load(i).cash == before);
    check(scalar("SELECT PurchaseState FROM CashPurchases WHERE PurchaseId = " +
                 std::to_string(purchase)) == 0);
  }
  const auto claim_failure = buy(4);
  const auto claim_before = load(4);
  rejects([&] {
    MySqlInventoryStore(configuration)
        .Pickup(players[4], {claim_failure + 1}, now + 1);
  });
  check(load(4).cash == claim_before.cash &&
        load(4).inventory.slots == claim_before.inventory.slots);
  check(refund(4, claim_failure).response.result == 1 && cash(4) == 2000000);

  for (int iteration = 0; iteration < 8; ++iteration) {
    reset(0);
    const auto purchase = buy(0);
    std::latch ready(2), go(1);
    std::array<std::uint32_t, 2> results{};
    std::array<std::exception_ptr, 2> errors{};
    const auto race = [&](std::size_t lane) {
      ready.count_down();
      go.wait();
      try {
        MySqlInventoryStore connection(configuration);
        results[lane] =
            lane == 0 || iteration % 2 == 0
                ? connection
                      .Refund(players[0],
                              {{}, purchase + static_cast<std::uint32_t>(lane)},
                              now + 1)
                      .response.result
                : connection.Pickup(players[0], {purchase + 1}, now + 1)
                      .response.result;
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
    const auto after = load(0);
    if (iteration % 2 == 0 || results[0] == 1) {
      check(cash(0) == 2000000 && after.cash.slots.empty() &&
            after.inventory.slots.empty());
    } else {
      check(cash(0) == 1000001 && after.cash.slots.size() == 1 &&
            after.cash.purchases[0].claimed);
      check(after.inventory.slots.size() == 1 &&
            after.inventory.slots[0].quantity == 34465);
      check(refund(0, purchase).response.result == 2);
    }
  }
  std::cout << "Cash refund persistence: " << checks << " checks passed.\n";
  return checks;
}
}  // namespace bubble_fighter::server::tests
