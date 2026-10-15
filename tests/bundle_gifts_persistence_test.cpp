#include <array>
#include <exception>
#include <iostream>
#include <latch>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "features/authentication/credential_crypto.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_gift_store.h"
#include "persistence/my_sql_inventory_store.h"

namespace bubble_fighter::server::tests {
std::size_t RunBundleGiftPersistenceTests(
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
      throw std::runtime_error("Bundle gift persistence line " +
                               std::to_string(at.line()));
  };
  const auto rejects = [&](auto action) {
    bool refused = false;
    try {
      action();
    } catch (const std::exception&) {
      refused = true;
    }
    check(refused);
  };
  const auto sql = [&](const std::string& text) {
    MySqlConnection connection(configuration);
    std::istringstream commands(text);
    std::string command;
    while (std::getline(commands, command, ';')) {
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
  const auto password = authentication::CredentialCrypto::HashPassword(
      "Synthetic-bundle-gift-password");
  const std::array names{"bgift_owner",  "bgift_other",     "bgift_third",
                         "bgift_header", "bgift_component", "bgift_grant",
                         "bgift_bonus",  "bgift_state"};
  const std::array labels{u"BundleOwner",  u"BundleOther", u"BundleThird",
                          u"BundleHeader", u"BundlePart",  u"BundleGrant",
                          u"BundleBonus",  u"BundleState"};
  std::array<characters::CharacterRecord, 8> players;
  for (std::size_t i = 0; i < players.size(); ++i) {
    check(accounts.CreateAccount(names[i], password));
    players[i] = {0, accounts.FindAccount(names[i])->account_id, labels[i], 10,
                  7};
    check(characters.Create(players[i]) ==
          characters::CharacterCreateResult::kCreated);
  }
  constexpr std::uint64_t now = 1800000000;
  const auto id = [&](std::size_t i) {
    return std::to_string(players[i].character_id);
  };
  const auto reset = [&] {
    sql("DELETE FROM GiftBundleComponents; DELETE FROM GiftBundles; DELETE FROM Gifts; DELETE FROM CashBundleComponents; DELETE FROM CashBundles; DELETE FROM CashInventorySlots; DELETE FROM CashPurchases; DELETE FROM InventorySlots; UPDATE Characters SET Cash=2000000,Lucci=10000; DELETE FROM SocialChatBlocks;");
  };
  const auto execute = [&](std::size_t i, const gifts::GiftRequest& request,
                           std::uint64_t time = 1800000000) {
    return MySqlGiftStore(configuration).Execute(players[i], request, time);
  };
  const auto send = [&](std::size_t from, std::size_t to,
                        std::uint32_t stock = 2000001) {
    return execute(from, GiftSendRequest{
                             players[to].character_name, {}, stock, u"Bundle"});
  };
  const auto cash = [&](std::size_t i) {
    return characters.FindByAccount(players[i].account_id)->statistics.cash;
  };
  const auto lucci = [&](std::size_t i) {
    return characters.FindByAccount(players[i].account_id)->statistics.lucci;
  };
  const auto quantity = [&](std::size_t i) {
    return scalar(
        "SELECT COALESCE(SUM(Quantity),0) FROM InventorySlots WHERE ItemId=100081 AND CharacterId=" +
        id(i));
  };
  reset();
  auto delivery = send(0, 1);
  check(delivery.changed && cash(0) == 1995500 && lucci(1) == 10000 &&
        quantity(1) == 0);
  auto gift = delivery.changed->gift_id;
  check(scalar("SELECT ComponentCount FROM GiftBundles WHERE GiftId=" +
               std::to_string(gift)) == 1);
  check(!execute(0, GiftReceiveRequest{gift}).changed &&
        !execute(2, GiftReceiveRequest{gift}).changed);
  check(execute(0, GiftDeleteSentRequest{gift}).changed.has_value());
  check(execute(1, GiftReceiveRequest{gift}, now + 10).changed.has_value());
  check(quantity(1) == 100 && lucci(1) == 70000 && cash(0) == 1995500);
  check(!execute(1, GiftReceiveRequest{gift}, now + 10).changed &&
        !execute(1, GiftDiscardRequest{gift}, now + 10).changed);
  check(scalar("SELECT COUNT(*) FROM GiftBundleComponents") == 1);
  check(scalar("SELECT COUNT(*) FROM CashPurchases") == 0 &&
        scalar("SELECT COUNT(*) FROM CashBundles") == 0 &&
        scalar("SELECT COUNT(*) FROM CashInventorySlots") == 0);
  check(MySqlInventoryStore(configuration)
            .Refund(players[1], {{}, gift}, now + 10)
            .response.result != 1);
  reset();
  delivery = send(0, 1, 2000105);
  gift = delivery.changed->gift_id;
  check(scalar("SELECT COUNT(*) FROM GiftBundleComponents") == 4);
  check(execute(1, GiftReceiveRequest{gift}, now + 20).changed.has_value());
  check(scalar("SELECT COUNT(*) FROM InventorySlots WHERE CharacterId=" +
               id(1)) == 4);
  check(scalar("SELECT COUNT(*) FROM InventorySlots WHERE CharacterId=" +
               id(1) + " AND ExpiresAtUnixSeconds=1800604820") == 3);
  check(execute(1, GiftSendThanksRequest{gift, u"Thanks", 4}, now + 20)
            .changed.has_value());
  check(GiftViewThanksResponse::Decode(
            execute(0, GiftViewThanksRequest{gift}, now + 20).packets[0])
            .text_at32 == u"Thanks");
  reset();
  delivery = send(0, 1, 2000107);
  gift = delivery.changed->gift_id;
  check(cash(0) == 2000000 && lucci(0) == 5500);
  check(execute(1, GiftReceiveRequest{gift}, now + 1).changed.has_value());
  check(scalar("SELECT COUNT(*) FROM InventorySlots WHERE CharacterId=" +
               id(1)) == 8);
  reset();
  delivery = send(0, 1);
  gift = delivery.changed->gift_id;
  sql("UPDATE GiftBundles SET BonusLucci=1234 WHERE GiftId=" +
      std::to_string(gift) +
      "; UPDATE GiftBundleComponents SET Amount=1 WHERE GiftId=" +
      std::to_string(gift) +
      "; UPDATE Gifts SET Price=1 WHERE GiftId=" + std::to_string(gift));
  check(execute(1, GiftReceiveRequest{gift}).changed.has_value());
  check(quantity(1) == 1 && lucci(1) == 11234);
  reset();
  gift = send(0, 1).changed->gift_id;
  sql("UPDATE Characters SET Lucci=2147483647 WHERE CharacterId=" + id(1));
  check(!execute(1, GiftReceiveRequest{gift}).changed && quantity(1) == 0);
  sql("UPDATE Characters SET Lucci=10000 WHERE CharacterId=" + id(1));
  check(execute(1, GiftReceiveRequest{gift}).changed.has_value());
  reset();
  gift = send(0, 1, 2000105).changed->gift_id;
  sql("DELETE FROM GiftBundleComponents WHERE GiftId=" + std::to_string(gift) +
      " AND ComponentIndex=2");
  rejects([&] { execute(1, GiftReceiveRequest{gift}); });
  check(scalar("SELECT GiftState FROM Gifts WHERE GiftId=" +
               std::to_string(gift)) == 0 &&
        scalar("SELECT COUNT(*) FROM InventorySlots") == 0);
  reset();
  gift = send(0, 1).changed->gift_id;
  sql("DELETE FROM GiftBundleComponents; DELETE FROM GiftBundles;");
  rejects([&] { execute(1, GiftReceiveRequest{gift}); });
  reset();
  gift = send(0, 1).changed->gift_id;
  check(!execute(1, GiftReceiveRequest{gift}, now + gifts::kGiftLifetimeSeconds)
             .changed);
  check(execute(1, GiftDeleteReceivedRequest{gift},
                now + gifts::kGiftLifetimeSeconds)
            .changed.has_value());
  // Failure boundaries: header and later component insertion, partial grants,
  // bonus credit, and final state transition all roll back the complete
  // operation.
  for (const auto from : {3U, 4U}) {
    reset();
    const auto sequence =
        scalar("SELECT NextGiftId FROM GiftSequence WHERE SequenceId=1");
    rejects([&] { send(from, 1, 2000105); });
    check(cash(from) == 2000000 && scalar("SELECT COUNT(*) FROM Gifts") == 0 &&
          scalar("SELECT COUNT(*) FROM GiftBundles") == 0 &&
          scalar("SELECT COUNT(*) FROM GiftBundleComponents") == 0);
    check(scalar("SELECT NextGiftId FROM GiftSequence WHERE SequenceId=1") ==
          sequence);
  }
  for (const auto to : {5U, 6U, 7U}) {
    reset();
    gift = send(0, to, to == 5 ? 2000105 : 2000001).changed->gift_id;
    rejects([&] { execute(to, GiftReceiveRequest{gift}); });
    check(scalar("SELECT COUNT(*) FROM InventorySlots WHERE CharacterId=" +
                 id(to)) == 0 &&
          lucci(to) == 10000 &&
          scalar("SELECT GiftState FROM Gifts WHERE GiftId=" +
                 std::to_string(gift)) == 0);
  }
  const auto race = [&](auto left, auto right) {
    std::latch ready(2), go(1);
    std::array<gifts::GiftResult, 2> results;
    std::array<std::exception_ptr, 2> errors;
    const auto run = [&](std::size_t index, auto action) {
      ready.count_down();
      go.wait();
      try {
        results[index] = action();
      } catch (...) {
        errors[index] = std::current_exception();
      }
    };
    std::thread first(run, 0, left), second(run, 1, right);
    ready.wait();
    go.count_down();
    first.join();
    second.join();
    for (const auto& error : errors)
      if (error) std::rethrow_exception(error);
    return results;
  };
  for (int i = 0; i < 3; ++i) {
    reset();
    gift = send(0, 1).changed->gift_id;
    auto results = race([&] { return execute(1, GiftReceiveRequest{gift}); },
                        [&] { return execute(1, GiftReceiveRequest{gift}); });
    check(bool(results[0].changed) != bool(results[1].changed) &&
          quantity(1) == 100 && lucci(1) == 70000);
    reset();
    gift = send(0, 1).changed->gift_id;
    results = race([&] { return execute(1, GiftReceiveRequest{gift}); },
                   [&] { return execute(1, GiftDiscardRequest{gift}); });
    check(bool(results[0].changed) != bool(results[1].changed));
    check(quantity(1) == (results[0].changed ? 100U : 0U) &&
          lucci(1) == (results[0].changed ? 70000U : 10000U));
    reset();
    results = race([&] { return send(0, 1); }, [&] { return send(2, 1); });
    check(bool(results[0].changed) != bool(results[1].changed) &&
          cash(0) + cash(2) == 3995500);
    check(scalar("SELECT COUNT(*) FROM Gifts") == 1);
    reset();
    results = race([&] { return send(0, 1); }, [&] { return send(1, 0); });
    check(results[0].changed && results[1].changed && cash(0) == 1995500 &&
          cash(1) == 1995500);
  }
  reset();
  std::cout << "Bundle gift persistence: " << checks
            << " checks passed; 12 real races.\n";
  return checks;
}
}  // namespace bubble_fighter::server::tests
