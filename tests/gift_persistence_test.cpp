#include <array>
#include <exception>
#include <iostream>
#include <latch>
#include <source_location>
#include <sstream>
#include <thread>

#include "features/authentication/credential_crypto.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_gift_store.h"
#include "persistence/my_sql_inventory_store.h"

namespace bubble_fighter::server::tests {
std::size_t RunGiftPersistenceTests(
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
      throw std::runtime_error("Gift persistence line " +
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
  const auto sql = [&](const std::string& command) {
    MySqlConnection connection(configuration);
    std::istringstream commands(command);
    std::string part;
    while (std::getline(commands, part, ';')) {
      if (part.find_first_not_of(' ') == std::string::npos) continue;
      Statement statement(connection.Handle(), part);
      statement.Execute({});
    }
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
  check(scalar("SELECT DATABASE() REGEXP '^BfGiftTests_[0-9a-f]{12}$'") == 1);
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  const auto password =
      authentication::CredentialCrypto::HashPassword("Synthetic-gift-password");
  const std::array names{"gift_owner",      "gift_other",
                         "gift_third",      "gift_fail_insert",
                         "gift_fail_claim", "gift_fail_thanks"};
  const std::array labels{u"GiftOwner",  u"GiftOther", u"GiftThird",
                          u"GiftInsert", u"GiftClaim", u"GiftThanks"};
  std::array<characters::CharacterRecord, 6> players;
  for (std::size_t i = 0; i < players.size(); ++i) {
    check(accounts.CreateAccount(names[i], password));
    players[i] = {0, accounts.FindAccount(names[i])->account_id, labels[i], 10,
                  7};
    check(characters.Create(players[i]) ==
          characters::CharacterCreateResult::kCreated);
    sql("UPDATE Characters SET Cash=2000000,Lucci=10000 WHERE CharacterId=" +
        std::to_string(players[i].character_id));
  }
  constexpr std::uint64_t now = 1800000000;
  const auto id = [&](std::size_t i) {
    return std::to_string(players[i].character_id);
  };
  const auto balance = [&](std::size_t i) {
    return characters.FindByAccount(players[i].account_id)->statistics.cash;
  };
  const auto execute = [&](std::size_t i, const gifts::GiftRequest& request,
                           std::uint64_t time = 1800000000) {
    return MySqlGiftStore(configuration).Execute(players[i], request, time);
  };
  const auto send = [&](std::size_t from, std::size_t to,
                        std::uint32_t stock = 1000139) {
    return execute(
        from, GiftSendRequest{players[to].character_name, {}, stock, u"Enjoy"});
  };
  const auto rows = [&](std::size_t i) {
    return GiftReceivedListResponse::Decode(
        execute(i, GiftReceivedListRequest{0, 4}).packets[0]);
  };
  const auto quantity = [&](std::size_t i) {
    return scalar(
        "SELECT COALESCE(SUM(Quantity),0) FROM InventorySlots WHERE ItemId=205034 AND CharacterId=" +
        id(i));
  };
  const auto reset = [&] {
    sql("DELETE FROM GiftBundleComponents; DELETE FROM GiftBundles; DELETE FROM Gifts; DELETE FROM CashInventorySlots; DELETE FROM CashPurchases; DELETE FROM InventorySlots; UPDATE Characters SET Cash=2000000,Lucci=10000; DELETE FROM SocialChatBlocks;");
  };
  auto sent = send(0, 1);
  check(sent.changed.has_value() && balance(0) == 1999000 && quantity(1) == 0);
  auto gift = sent.changed->gift_id;
  check(rows(1).records[0].gift_id == gift && rows(0).records.empty());
  check(MySqlGiftStore(configuration).Pending(players[1], now) == 1);
  check(!execute(2, GiftReceiveRequest{gift}).changed);
  check(!execute(0, GiftReceiveRequest{gift}).changed);
  check(!execute(1, GiftDeleteSentRequest{gift}).changed);
  auto forged = players[1];
  forged.account_id = players[2].account_id;
  rejects([&] {
    MySqlGiftStore(configuration)
        .Execute(forged, GiftReceiveRequest{gift}, now);
  });
  forged = players[1];
  forged.character_name = u"StaleName";
  rejects([&] {
    MySqlGiftStore(configuration)
        .Execute(forged, GiftReceiveRequest{gift}, now);
  });
  auto hidden = execute(0, GiftDeleteSentRequest{gift});
  check(hidden.changed->sender_hidden && rows(1).records.size() == 1);
  check(execute(1, GiftReceiveRequest{gift}, now + 1).changed.has_value() &&
        quantity(1) == 100);
  check(!execute(1, GiftReceiveRequest{gift}, now + 1).changed);
  check(!execute(1, GiftDiscardRequest{gift}, now + 1).changed);
  check(MySqlGiftStore(configuration).Pending(players[1], now + 1) == 0);
  check(scalar("SELECT COUNT(*) FROM CashPurchases") == 0 &&
        scalar("SELECT COUNT(*) FROM CashInventorySlots") == 0);
  check(execute(1, GiftSendThanksRequest{gift, u"Thank you", 4}, now + 1)
            .changed.has_value());
  check(!execute(1, GiftSendThanksRequest{gift, u"Again", 2}, now + 1).changed);
  // Sender-hidden records remain hidden even when a later thanks is persisted.
  check(GiftViewThanksResponse::Decode(
            execute(0, GiftViewThanksRequest{gift}, now + 1).packets[0])
            .value_at0 == 0);
  check(
      execute(1, GiftDeleteReceivedRequest{gift}, now + 1).changed.has_value());
  check(rows(1).records.empty());
  reset();
  sent = send(0, 1);
  gift = sent.changed->gift_id;
  execute(1, GiftReceiveRequest{gift}, now + 1);
  execute(1, GiftSendThanksRequest{gift, u"Saved thanks", 6}, now + 1);
  auto thanks = GiftViewThanksResponse::Decode(
      execute(0, GiftViewThanksRequest{gift}, now + 1).packets[0]);
  check(thanks.value_at0 == 1 && thanks.text_at8 == u"GiftOther" &&
        thanks.text_at32 == u"Saved thanks");
  check(thanks.value_at56 == 6 && thanks.value_at60 == 7 &&
        thanks.extra.value_at0 == 300010);
  check(
      execute(1, GiftDeleteReceivedRequest{gift}, now + 1).changed.has_value());
  check(GiftViewThanksResponse::Decode(
            execute(0, GiftViewThanksRequest{gift}, now + 1).packets[0]) ==
        thanks);
  reset();
  sent = send(0, 1);
  gift = sent.changed->gift_id;
  check(!execute(1, GiftReceiveRequest{gift}, now + 2592000).changed &&
        quantity(1) == 0);
  check(MySqlGiftStore(configuration).Pending(players[1], now + 2592000) == 0);
  check(execute(1, GiftDeleteReceivedRequest{gift}, now + 2592000)
            .changed.has_value());
  check(balance(0) == 1999000);
  reset();
  check(GiftSendResponse::Decode(send(0, 0).packets[0]).result ==
        std::uint32_t(-18));
  check(GiftSendResponse::Decode(
            execute(0, GiftSendRequest{u"Missing", {}, 1000139, {}}).packets[0])
            .result == std::uint32_t(-1));
  for (auto blocked : {false, true}) {
    sql("INSERT INTO SocialChatBlocks (OwnerCharacterId,TargetCharacterId) VALUES (" +
        id(blocked ? 1 : 0) + "," + id(blocked ? 0 : 1) + ")");
    check(!send(0, 1).changed && balance(0) == 2000000);
    sql("DELETE FROM SocialChatBlocks");
  }
  sql("UPDATE Accounts SET IsDisabled=1 WHERE AccountId=" +
      std::to_string(players[1].account_id));
  check(!send(0, 1).changed);
  rejects([&] { execute(1, GiftReceivedListRequest{0, 4}); });
  sql("UPDATE Accounts SET IsDisabled=0 WHERE AccountId=" +
      std::to_string(players[1].account_id));

  // Triggers fail after debit or grant has already been prepared/written.
  const auto sequence = scalar("SELECT NextGiftId FROM GiftSequence");
  rejects([&] { send(3, 1); });
  check(balance(3) == 2000000 && rows(1).records.empty());
  check(scalar("SELECT NextGiftId FROM GiftSequence") == sequence);
  sent = send(0, 4);
  gift = sent.changed->gift_id;
  rejects([&] { execute(4, GiftReceiveRequest{gift}, now + 1); });
  check(quantity(4) == 0 && rows(4).records[0].short_at48 == 0);
  sent = send(0, 5);
  gift = sent.changed->gift_id;
  execute(5, GiftReceiveRequest{gift}, now + 1);
  rejects([&] {
    execute(5, GiftSendThanksRequest{gift, u"Rollback", 2}, now + 1);
  });
  check(quantity(5) == 100);
  check(GiftViewThanksResponse::Decode(
            execute(0, GiftViewThanksRequest{gift}, now + 1).packets[0])
            .value_at0 == 0);
  reset();

  const auto race = [&](auto first, auto second) {
    std::latch ready(2), start(1);
    std::array<gifts::GiftResult, 2> results;
    std::array<std::exception_ptr, 2> errors;
    const auto worker = [&](std::size_t index, auto operation) {
      ready.count_down();
      start.wait();
      try {
        results[index] = operation();
      } catch (...) {
        errors[index] = std::current_exception();
      }
    };
    std::thread a(worker, 0, first), b(worker, 1, second);
    ready.wait();
    start.count_down();
    a.join();
    b.join();
    for (const auto& error : errors)
      if (error) std::rethrow_exception(error);
    return results;
  };
  for (unsigned round = 0; round < 3; ++round) {
    reset();
    gift = send(0, 1).changed->gift_id;
    auto results = race([&] { return execute(1, GiftReceiveRequest{gift}); },
                        [&] { return execute(1, GiftReceiveRequest{gift}); });
    check(bool(results[0].changed) != bool(results[1].changed));
    check(quantity(1) == 100 && balance(0) == 1999000);
    reset();
    gift = send(0, 1).changed->gift_id;
    results = race([&] { return execute(1, GiftReceiveRequest{gift}); },
                   [&] { return execute(1, GiftDiscardRequest{gift}); });
    check(bool(results[0].changed) != bool(results[1].changed));
    check(quantity(1) == (results[0].changed ? 100 : 0));
    reset();
    sql("UPDATE Characters SET Cash=1000 WHERE CharacterId=" + id(0));
    results = race([&] { return send(0, 1); }, [&] { return send(0, 2); });
    check(bool(results[0].changed) != bool(results[1].changed));
    check(balance(0) == 0 && scalar("SELECT COUNT(*) FROM Gifts") == 1);
    reset();
    results = race([&] { return send(0, 1, 1000216); },
                   [&] { return send(2, 1, 1000216); });
    check(bool(results[0].changed) != bool(results[1].changed));
    check(scalar("SELECT COUNT(*) FROM Gifts") == 1 &&
          balance(0) + balance(2) == 3999999);
    reset();
    gift = send(0, 1).changed->gift_id;
    execute(1, GiftReceiveRequest{gift});
    results = race(
        [&] { return execute(1, GiftSendThanksRequest{gift, u"One", 2}); },
        [&] { return execute(1, GiftSendThanksRequest{gift, u"Two", 3}); });
    check(bool(results[0].changed) != bool(results[1].changed));
    check(GiftViewThanksResponse::Decode(
              execute(0, GiftViewThanksRequest{gift}).packets[0])
              .value_at0 == 1);
    reset();
    gift = send(0, 1).changed->gift_id;
    results = race([&] { return execute(0, GiftDeleteSentRequest{gift}); },
                   [&] { return execute(1, GiftReceiveRequest{gift}); });
    check(results[0].changed && results[1].changed && quantity(1) == 100);
    reset();
    results = race([&] { return send(0, 1); }, [&] { return send(1, 0); });
    check(results[0].changed && results[1].changed);
    check(balance(0) == 1999000 && balance(1) == 1999000 &&
          scalar("SELECT COUNT(*) FROM Gifts") == 2);
  }
  reset();
  // A pending permanent cash purchase reserves the same recipient capacity.
  check(MySqlInventoryStore(configuration)
            .Purchase(players[1], {1000216, 0}, now)
            .response.result == 1);
  check(!send(0, 1, 1000216).changed && balance(0) == 2000000);
  reset();
  auto empty =
      execute(0, GiftSendRequest{players[1].character_name, {}, 1000139, {}});
  check(empty.changed && empty.changed->message.empty());
  check(rows(1).records[0].text_at52.empty());
  check(!send(0, 1, 1000127).changed && balance(0) == 1999000);
  reset();
  std::cout << "Gift persistence: " << checks
            << " checks passed; 21 concurrent races.\n";
  return checks;
}
}  // namespace bubble_fighter::server::tests
