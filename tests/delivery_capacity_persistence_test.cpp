#include <array>
#include <exception>
#include <iostream>
#include <latch>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "persistence/inventory/inventory_rows.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_gift_store.h"
#include "persistence/my_sql_inventory_store.h"

namespace bubble_fighter::server::tests {
std::size_t RunDeliveryCapacityPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace my_sql_commands;
  using namespace features;
  using namespace protocol;
  std::size_t checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Delivery capacity persistence line " +
                               std::to_string(at.line()));
  };
  const auto sql = [&](const std::string& text) {
    MySqlConnection connection(configuration);
    std::istringstream stream(text);
    std::string command;
    while (std::getline(stream, command, ';')) {
      if (command.find_first_not_of(' ') == std::string::npos) continue;
      Statement statement(connection.Handle(), command);
      statement.Execute({});
    }
  };
  const auto scalar = [&](const std::string& text) {
    MySqlConnection connection(configuration);
    Statement query(connection.Handle(), text);
    query.Execute({});
    std::uint64_t value = 0;
    std::array columns{Number(value)};
    check(query.Fetch(columns));
    return value;
  };
  check(scalar("SELECT DATABASE() REGEXP '^BfGiftTests_[0-9a-f]{12}$'") == 1);
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  std::array<characters::CharacterRecord, 3> players;
  const std::array names{"bgift_owner", "bgift_other", "bgift_third"};
  for (std::size_t i = 0; i < players.size(); ++i)
    players[i] =
        characters.FindByAccount(accounts.FindAccount(names[i])->account_id)
            .value();
  constexpr std::uint64_t now = 1800000000;
  const auto id = [&](std::size_t i) {
    return std::to_string(players[i].character_id);
  };
  const auto current = [&](std::size_t i) {
    return characters.FindByAccount(players[i].account_id).value();
  };
  const auto reset = [&] {
    sql("DELETE FROM GiftBundleComponents; DELETE FROM GiftBundles; DELETE FROM Gifts; DELETE FROM CashBundleComponents; DELETE FROM CashBundles; DELETE FROM CashInventorySlots; DELETE FROM CashPurchases; DELETE FROM InventorySlots; UPDATE Characters SET Cash=2000000,Lucci=10000; DELETE FROM SocialChatBlocks;");
  };
  const auto action = [&](std::size_t i, const gifts::GiftRequest& request) {
    return MySqlGiftStore(configuration).Execute(players[i], request, now);
  };
  const auto send = [&](std::uint32_t stock = 2000001, std::size_t to = 1) {
    return action(
        0, GiftSendRequest{players[to].character_name, {}, stock, u"Capacity"});
  };
  const auto buy = [&](std::uint32_t stock = 2000001, std::size_t who = 1) {
    return MySqlInventoryStore(configuration)
        .Purchase(players[who], {stock, 0}, now);
  };
  const auto refund = [&](std::uint32_t slot) {
    return MySqlInventoryStore(configuration)
        .Refund(players[1], {{}, slot}, now);
  };
  const auto pickup = [&](std::uint32_t slot) {
    return MySqlInventoryStore(configuration).Pickup(players[1], {slot}, now);
  };
  const auto deny_buy = [&](std::uint32_t stock) {
    const auto before = current(1);
    const auto sequence = scalar(
        "SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId=1");
    const auto result = buy(stock);
    check(result.response.result == ShopPurchaseResponse::kIneligible &&
          result.character == before && result.changed_item_id == 0 &&
          result.lucci_cost == 0 && result.packets.size() == 1 &&
          result.response.value_at40 == 0);
    check(
        current(1) == before &&
        scalar(
            "SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId=1") ==
            sequence);
  };
  const auto duration = [&](std::uint32_t item, std::uint32_t days) {
    MySqlConnection connection(configuration);
    Transaction transaction(connection.Handle());
    check(inventory_sql::LockOwner(connection.Handle(), players[1].account_id,
                                   players[1].character_id));
    inventory::InventorySlot slot;
    slot.item_id = item;
    slot.usage = inventory::InventoryUsage::kDuration;
    slot.quantity = 1;
    slot.expires_at_unix_seconds = now + std::uint64_t{days} * 86400;
    inventory_sql::PersistGrant(connection.Handle(), players[1], slot);
    transaction.Commit();
  };
  reset();
  auto gift = send().changed->gift_id;
  deny_buy(1000239);
  deny_buy(2000001);
  check(action(0, GiftDeleteSentRequest{gift}).changed.has_value());
  deny_buy(2000001);
  check(action(1, GiftDiscardRequest{gift}).changed.has_value());
  auto purchased = buy();
  check(purchased.response.result == 1);
  check(refund(purchased.response.value_at40).response.result == 1);
  check(send().changed.has_value());
  reset();
  purchased = buy();
  check(purchased.response.result == 1);
  const auto gift_sequence =
      scalar("SELECT NextGiftId FROM GiftSequence WHERE SequenceId=1");
  check(!send().changed && current(0).statistics.cash == 2000000);
  check(scalar("SELECT NextGiftId FROM GiftSequence WHERE SequenceId=1") ==
        gift_sequence);
  check(refund(purchased.response.value_at40).response.result == 1);
  gift = send().changed->gift_id;
  check(action(1, GiftReceiveRequest{gift}).changed.has_value());
  reset();
  gift = send(1000217).changed->gift_id;
  deny_buy(1000216);
  check(action(1, GiftReceiveRequest{gift}).changed.has_value());
  purchased = buy(1000216);
  check(purchased.response.result == 1);
  check(pickup(purchased.response.value_at40).response.result == 1);
  check(!send(1000217).changed);
  reset();
  purchased = buy(1000216);
  check(purchased.response.result == 1);
  check(!send(1000217).changed);
  check(refund(purchased.response.value_at40).response.result == 1);
  check(send(1000217).changed.has_value());
  reset();
  duration(100002, 359);
  gift = send(1000004).changed->gift_id;
  deny_buy(1000004);
  check(
      scalar(
          "SELECT ExpiresAtUnixSeconds FROM InventorySlots WHERE CharacterId=" +
          id(1) + " AND ItemId=100002") == now + 359ULL * 86400);
  check(action(1, GiftDiscardRequest{gift}).changed.has_value());
  check(buy(1000004).response.result == 1 &&
        current(1).statistics.lucci == 9940);
  reset();
  duration(207029, 353);
  gift = send(2000107).changed->gift_id;
  deny_buy(2000107);
  check(scalar("SELECT COUNT(*) FROM InventorySlots WHERE CharacterId=" +
               id(1)) == 1);
  check(action(1, GiftDiscardRequest{gift}).changed.has_value());
  check(buy(2000107).response.result == 1 &&
        current(1).statistics.lucci == 5500);
  reset();
  sql("UPDATE Characters SET Lucci=2147423647 WHERE CharacterId=" + id(1));
  check(send().changed.has_value());
  deny_buy(2000002);
  check(current(1).statistics.lucci == 2147423647U);
  reset();
  sql("UPDATE Characters SET Lucci=2147423647 WHERE CharacterId=" + id(1));
  check(buy(2000002).response.result == 1);
  check(!send().changed);
  reset();
  gift = send().changed->gift_id;
  sql("UPDATE Gifts SET SentAtUnixSeconds=" +
      std::to_string(now - gifts::kGiftLifetimeSeconds) +
      " WHERE GiftId=" + std::to_string(gift));
  check(buy().response.result == 1 &&
        !action(1, GiftReceiveRequest{gift}).changed);
  reset();
  check(send(2000001, 2).changed.has_value());
  check(buy(2000001, 0).response.result == 1);
  // A clock rollback must not make a durable pending gift disappear from
  // admission, even while gift UI operations wait for its sent timestamp.
  reset();
  gift = send().changed->gift_id;
  sql("UPDATE Gifts SET SentAtUnixSeconds=" + std::to_string(now + 60) +
      " WHERE GiftId=" + std::to_string(gift));
  deny_buy(2000001);
  reset();
  gift = send(2000105).changed->gift_id;
  sql("DELETE FROM GiftBundleComponents WHERE ComponentIndex=2 AND GiftId=" +
      std::to_string(gift));
  const auto before_corruption = current(1);
  const auto before_sequence =
      scalar("SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId=1");
  bool corruption_refused = false;
  try {
    (void)buy(1000139);
  } catch (const std::exception&) {
    corruption_refused = true;
  }
  check(
      corruption_refused && current(1) == before_corruption &&
      scalar(
          "SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId=1") ==
          before_sequence);
  // Claiming either compatible source first consumes exactly its reservation.
  for (int first = 0; first < 2; ++first) {
    reset();
    gift = send(1000139).changed->gift_id;
    purchased = buy(1000139);
    check(purchased.response.result == 1);
    if (first) check(action(1, GiftReceiveRequest{gift}).changed.has_value());
    check(pickup(purchased.response.value_at40).response.result == 1);
    if (!first) check(action(1, GiftReceiveRequest{gift}).changed.has_value());
    check(scalar("SELECT Quantity FROM InventorySlots WHERE CharacterId=" +
                 id(1) + " AND ItemId=205034") == 200);
  }
  const auto race = [&](auto left, auto right) {
    std::latch ready(2), go(1);
    std::array<bool, 2> result{};
    std::array<std::exception_ptr, 2> errors;
    const auto run = [&](std::size_t i, auto task) {
      ready.count_down();
      go.wait();
      try {
        result[i] = task();
      } catch (...) {
        errors[i] = std::current_exception();
      }
    };
    std::thread a(run, 0, left), b(run, 1, right);
    ready.wait();
    go.count_down();
    a.join();
    b.join();
    for (const auto& error : errors)
      if (error) std::rethrow_exception(error);
    return result;
  };
  for (int round = 0; round < 3; ++round) {
    for (const auto stock : {1000239U, 2000001U}) {
      reset();
      const auto result = race([&] { return send().changed.has_value(); },
                               [&] { return buy(stock).response.result == 1; });
      check(result[0] != result[1]);
      check(scalar("SELECT COUNT(*) FROM Gifts") +
                scalar("SELECT COUNT(*) FROM CashInventorySlots") +
                scalar("SELECT COUNT(*) FROM CashBundles") ==
            1);
    }
    reset();
    duration(100002, 359);
    auto result = race([&] { return send(1000004).changed.has_value(); },
                       [&] { return buy(1000004).response.result == 1; });
    check(result[0] != result[1]);
    reset();
    purchased = buy(1000239);
    check(purchased.response.result == 1);
    result = race(
        [&] { return send().changed.has_value(); },
        [&] {
          return refund(purchased.response.value_at40).response.result == 1;
        });
    check(result[1]);
    if (!result[0]) check(send().changed.has_value());
    check(scalar("SELECT COUNT(*) FROM Gifts") == 1 &&
          scalar("SELECT COUNT(*) FROM CashInventorySlots") == 0 &&
          current(1).statistics.cash == 2000000);
  }
  reset();
  std::cout << "Delivery capacity persistence: " << checks
            << " checks passed; 12 cross-system races.\n";
  return checks;
}
}  // namespace bubble_fighter::server::tests
