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
#include "persistence/my_sql_social_store.h"

namespace bubble_fighter::server::tests {
std::size_t RunOfflineMessagePersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace my_sql_commands;
  using namespace features;
  using namespace protocol;
  using Status = CompanionSocialStatus;
  using Action = CompanionSocialAction;
  std::size_t checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Mailbox persistence line " +
                               std::to_string(at.line()));
  };
  const auto rejects = [&](auto action) {
    bool failed = false;
    try {
      action();
    } catch (const std::exception&) {
      failed = true;
    }
    check(failed);
  };
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  const auto password = authentication::CredentialCrypto::HashPassword(
      "Synthetic-mailbox-password");
  std::array<characters::CharacterRecord, 3> players;
  const std::array users{"mail_alice", "mail_bob", "mail_carol"};
  const std::array names{u"MailAlice", u"MailBob", u"MailCarol"};
  for (std::size_t i = 0; i < players.size(); ++i) {
    check(accounts.CreateAccount(users[i], password));
    players[i] = {0, accounts.FindAccount(users[i])->account_id, names[i], 27,
                  2};
    check(characters.Create(players[i]) ==
          characters::CharacterCreateResult::kCreated);
  }
  const auto id = [&](std::size_t i) {
    return static_cast<std::uint32_t>(players[i].character_id);
  };
  constexpr std::uint64_t now = 1792000000;
  const auto send = [&](std::size_t from, std::size_t to,
                        std::u16string text = u"留言😀",
                        std::uint64_t clock = 1792000000) {
    return MySqlSocialStore(configuration)
        .QueueMessage(players[from],
                      {Action::kMessage, id(to), {}, std::move(text)}, clock);
  };
  const auto load = [&](std::size_t who, std::uint64_t clock = 1792000000) {
    return MySqlSocialStore(configuration).LoadMailbox(players[who], clock);
  };
  const auto ack = [&](std::size_t who, std::uint64_t message) {
    MySqlSocialStore(configuration).AcknowledgeMessage(players[who], message);
  };
  const auto relationship = [&](std::size_t owner, Action action,
                                std::size_t peer) {
    return MySqlSocialStore(configuration)
        .Change(players[owner], {action, id(peer), {}, {}})
        .status;
  };
  const auto sql = [&](const std::string& statement) {
    MySqlConnection connection(configuration);
    Statement query(connection.Handle(), statement);
    query.Execute({});
  };
  const auto count = [&](std::size_t owner) {
    MySqlConnection connection(configuration);
    Statement query(
        connection.Handle(),
        "SELECT COUNT(*) FROM SocialMessages WHERE RecipientCharacterId = ?");
    std::array parameters{Number(players[owner].character_id)};
    query.Execute(parameters);
    std::uint64_t result = 0;
    std::array columns{Number(result)};
    check(query.Fetch(columns));
    return result;
  };
  const auto clear = [&](std::size_t owner) {
    for (;;) {
      auto mailbox = load(owner);
      if (mailbox.messages.empty()) break;
      for (const auto& message : mailbox.messages)
        ack(owner, message.message_id);
    }
  };
  check(load(0).messages.empty());
  check(send(0, 0).status == Status::kSelf);
  check(send(0, 1).status == Status::kNotFriend);
  check(relationship(0, Action::kAdd, 1) == Status::kOk);
  check(send(0, 1).status == Status::kNotFriend);
  check(relationship(1, Action::kAccept, 0) == Status::kOk);
  auto sent = send(0, 1);
  check(sent.status == Status::kOk && sent.message_id > 9007199254740992ULL);
  check(sent.target->name == u"MailBob" && sent.sender_name == u"MailAlice");
  auto mailbox = load(1);
  check(mailbox.status == CompanionMailboxStatus::kOk &&
        mailbox.messages.size() == 1);
  check(mailbox.messages[0] == CompanionQueuedMessage{sent.message_id, id(0),
                                                      now, u"MailAlice",
                                                      u"留言😀"});
  check(load(0).messages.empty() && load(2).messages.empty());
  check(load(1).messages ==
        mailbox.messages);  // New connection/store; no destructive read.
  ack(0, sent.message_id);
  ack(2, sent.message_id);
  check(load(1).messages == mailbox.messages);
  auto stolen = players[1];
  stolen.account_id = players[0].account_id;
  rejects([&] { MySqlSocialStore(configuration).LoadMailbox(stolen, now); });
  rejects([&] {
    MySqlSocialStore(configuration).AcknowledgeMessage(stolen, sent.message_id);
  });
  rejects([&] {
    MySqlSocialStore(configuration)
        .QueueMessage(stolen, {Action::kMessage, id(0), {}, u"stolen"}, now);
  });
  rejects([&] { ack(1, 0); });
  rejects([&] { load(1, 0); });
  rejects([&] {
    send(0, 1, u"invalid clock",
         CompanionMailboxNotification::kMaximumTimestamp + 1);
  });
  check(send(0, 1, u" \u3000").status == Status::kInvalid);
  check(send(0, 1, u"bad\ntext").status == Status::kInvalid);
  check(MySqlSocialStore(configuration)
            .QueueMessage(players[0],
                          {Action::kMessage, id(1), u"Wrong", u"no"}, now)
            .status == Status::kNotFound);
  ack(1, sent.message_id);
  ack(1, sent.message_id);
  check(load(1).messages.empty());
  check(send(0, 1).status == Status::kOk);
  sql("UPDATE Characters SET NameUtf16 = X'4D00610069006C00520065006E0061006D0065006400' WHERE CharacterId = " +
      std::to_string(id(0)));
  check(load(1).messages[0].sender_name == u"MailRenamed");
  check(send(0, 1).sender_name == u"MailRenamed");
  sql("UPDATE Accounts SET IsDisabled = 1 WHERE AccountId = " +
      std::to_string(players[0].account_id));
  check(load(1).messages.empty() && count(1) == 2);
  rejects([&] { send(0, 1); });
  rejects([&] { send(1, 0); });
  rejects([&] { load(0); });
  sql("UPDATE Accounts SET IsDisabled = 0 WHERE AccountId = " +
      std::to_string(players[0].account_id));
  check(load(1).messages.size() == 2);
  check(relationship(0, Action::kBlock, 1) == Status::kOk);
  check(count(1) == 0 && send(0, 1).status == Status::kBlocked &&
        send(1, 0).status == Status::kBlocked);
  check(relationship(0, Action::kUnblock, 1) == Status::kOk);
  check(load(1).messages.empty());
  check(send(0, 1).status == Status::kOk && send(1, 0).status == Status::kOk);
  check(relationship(1, Action::kBlock, 0) == Status::kOk);
  check(count(0) == 0 && count(1) == 0);
  check(relationship(1, Action::kUnblock, 0) == Status::kOk);
  check(send(0, 1).status == Status::kOk && send(1, 0).status == Status::kOk);
  check(relationship(0, Action::kRemove, 1) == Status::kOk);
  check(count(0) == 0 && count(1) == 0);
  check(relationship(0, Action::kAdd, 1) == Status::kOk &&
        relationship(1, Action::kAccept, 0) == Status::kOk);
  const auto expiring = send(0, 1);
  check(expiring.status == Status::kOk);
  check(load(1, now + social::kMessageLifetimeSeconds - 1).messages.size() ==
        1);
  check(load(1, now + social::kMessageLifetimeSeconds).messages.empty() &&
        count(1) == 0);
  check(send(0, 1, u"future", now + 1).status == Status::kOk);
  check(load(1).messages.empty());
  check(load(1, now + 1).messages.size() == 1);
  sql("DELETE FROM SocialMessages WHERE RecipientCharacterId = " +
      std::to_string(id(1)));
  rejects([&] { send(0, 1, u"FailInsert"); });
  check(count(1) == 0);
  const auto failure = send(0, 1, u"FailAck");
  check(failure.status == Status::kOk);
  rejects([&] { ack(1, failure.message_id); });
  check(load(1).messages.size() == 1);
  rejects([&] { relationship(1, Action::kBlock, 0); });
  check(MySqlSocialStore(configuration).CanMessage(players[0], id(1)) ==
        Status::kOk);
  check(load(1).messages.size() == 1);
  rejects([&] { load(1, now + social::kMessageLifetimeSeconds); });
  check(count(1) == 1);
  sql("UPDATE SocialMessages SET TextUtf16 = X'6F006B00' WHERE MessageId = " +
      std::to_string(failure.message_id));
  clear(1);
  check(relationship(2, Action::kAdd, 1) == Status::kOk &&
        relationship(1, Action::kAccept, 2) == Status::kOk);
  for (int i = 0; i < 199; ++i) check(send(0, 1).status == Status::kOk);
  std::latch ready(2), go(1);
  std::array<Status, 2> results{};
  std::array<std::exception_ptr, 2> failures{};
  const auto contender = [&](std::size_t index, std::size_t sender) {
    ready.count_down();
    go.wait();
    try {
      results[index] = send(sender, 1).status;
    } catch (...) {
      failures[index] = std::current_exception();
    }
  };
  std::thread a(contender, 0, 0), c(contender, 1, 2);
  ready.wait();
  go.count_down();
  a.join();
  c.join();
  for (auto exception : failures)
    if (exception) std::rethrow_exception(exception);
  check((results[0] == Status::kOk && results[1] == Status::kCapacity) ||
        (results[1] == Status::kOk && results[0] == Status::kCapacity));
  check(count(1) == 200 && load(1).messages.size() == 20);
  check(send(0, 1).status == Status::kCapacity &&
        send(2, 1).status == Status::kCapacity);
  check(send(0, 1, u"after expiry", now + social::kMessageLifetimeSeconds)
            .status == Status::kOk);
  check(count(1) == 1);
  sql("DELETE FROM SocialMessages WHERE RecipientCharacterId = " +
      std::to_string(id(1)));
  // A concurrent block must remove even a send that committed just before it.
  std::latch race(1);
  Status send_result = Status::kInvalid, block_result = Status::kInvalid;
  failures = {};
  std::thread sender([&] {
    race.wait();
    try {
      send_result = send(0, 1).status;
    } catch (...) {
      failures[0] = std::current_exception();
    }
  });
  std::thread blocker([&] {
    race.wait();
    try {
      block_result = relationship(1, Action::kBlock, 0);
    } catch (...) {
      failures[1] = std::current_exception();
    }
  });
  race.count_down();
  sender.join();
  blocker.join();
  for (auto exception : failures)
    if (exception) std::rethrow_exception(exception);
  check(block_result == Status::kOk &&
        (send_result == Status::kOk || send_result == Status::kBlocked));
  check(count(1) == 0 && load(1).messages.empty());
  std::cout << "Offline message persistence: " << checks << " checks passed\n";
  return checks;
}
}  // namespace bubble_fighter::server::tests
