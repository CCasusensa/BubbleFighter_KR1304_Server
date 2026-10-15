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
std::size_t RunSocialPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace my_sql_commands;
  using namespace features;
  using namespace protocol;
  std::size_t checks = 0;
  const auto check = [&](bool ok, const std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok) {
      throw std::runtime_error("Social persistence line " +
                               std::to_string(at.line()));
    }
  };
  const auto rejects = [&](const auto& action) {
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
      "Synthetic-social-password");
  std::array<characters::CharacterRecord, 3> players;
  const std::array names{u"SocialAlice", u"SocialBob", u"SocialCarol"};
  const std::array users{"social_alice", "social_bob", "social_carol"};
  for (std::size_t i = 0; i < players.size(); ++i) {
    check(accounts.CreateAccount(users[i], password));
    players[i] = {0, accounts.FindAccount(users[i])->account_id, names[i], 27,
                  2};
    check(characters.Create(players[i]) ==
          characters::CharacterCreateResult::kCreated);
  }
  const auto target = [&](std::size_t i) {
    return static_cast<std::uint32_t>(players[i].character_id);
  };
  const auto change = [&](std::size_t owner, CompanionSocialAction action,
                          std::size_t peer) {
    return MySqlSocialStore(configuration)
        .Change(players[owner], {action, target(peer), {}, {}})
        .status;
  };
  const auto snapshot = [&](std::size_t i) {
    return MySqlSocialStore(configuration).Load(players[i]);
  };
  const auto can = [&](std::size_t a, std::size_t b) {
    return MySqlSocialStore(configuration).CanMessage(players[a], target(b));
  };
  check(snapshot(0).rows.empty());
  check(can(0, 1) == CompanionSocialStatus::kNotFriend);
  check(change(0, CompanionSocialAction::kAdd, 0) ==
        CompanionSocialStatus::kSelf);
  check(MySqlSocialStore(configuration)
            .Change(players[0], {CompanionSocialAction::kAdd, 0, u"absent", {}})
            .status == CompanionSocialStatus::kNotFound);
  check(change(0, CompanionSocialAction::kAdd, 1) ==
        CompanionSocialStatus::kOk);
  check(snapshot(0).rows[0].relationship == 3 &&
        snapshot(1).rows[0].relationship == 2);
  check(change(0, CompanionSocialAction::kAccept, 1) ==
        CompanionSocialStatus::kNoRequest);
  check(change(1, CompanionSocialAction::kAdd, 0) ==
        CompanionSocialStatus::kOk);
  check(snapshot(0).rows[0].relationship ==
        3);  // reciprocal add is not acceptance
  check(change(1, CompanionSocialAction::kDecline, 0) ==
        CompanionSocialStatus::kOk);
  check(snapshot(0).rows.empty() && snapshot(1).rows.empty());
  check(change(0, CompanionSocialAction::kAdd, 1) ==
        CompanionSocialStatus::kOk);
  check(change(1, CompanionSocialAction::kAccept, 0) ==
        CompanionSocialStatus::kOk);
  check(can(0, 1) == CompanionSocialStatus::kOk &&
        can(1, 0) == CompanionSocialStatus::kOk);
  check(snapshot(0).rows[0].relationship == 1 &&
        snapshot(1).rows[0].relationship == 1);
  check(change(0, CompanionSocialAction::kBlock, 1) ==
        CompanionSocialStatus::kOk);
  check(can(0, 1) == CompanionSocialStatus::kBlocked &&
        can(1, 0) == CompanionSocialStatus::kBlocked);
  check(snapshot(0).blocks.size() == 1 &&
        snapshot(0).rows[0].relationship == 4);
  check(change(0, CompanionSocialAction::kBlock, 1) ==
            CompanionSocialStatus::kOk &&
        snapshot(0).blocks.size() == 1);
  check(change(0, CompanionSocialAction::kUnblock, 1) ==
        CompanionSocialStatus::kOk);
  check(can(0, 1) == CompanionSocialStatus::kOk &&
        snapshot(0).rows[0].relationship == 1);
  check(change(0, CompanionSocialAction::kRemove, 1) ==
        CompanionSocialStatus::kOk);
  check(snapshot(0).rows.empty() && snapshot(1).rows.empty());
  check(change(0, CompanionSocialAction::kAdd, 1) ==
        CompanionSocialStatus::kOk);
  check(change(1, CompanionSocialAction::kBlock, 0) ==
        CompanionSocialStatus::kOk);
  check(snapshot(0).rows.empty() && snapshot(1).blocks.size() == 1);
  check(change(0, CompanionSocialAction::kAdd, 1) ==
        CompanionSocialStatus::kBlocked);
  check(change(1, CompanionSocialAction::kUnblock, 0) ==
        CompanionSocialStatus::kOk);
  auto spoof = players[0];
  spoof.account_id = players[2].account_id;
  rejects([&] {
    MySqlSocialStore(configuration)
        .Change(spoof, {CompanionSocialAction::kAdd, target(1), {}, {}});
  });
  check(snapshot(0).rows.empty());
  // Opposite-direction concurrent requests share ordered character locks and
  // one pair key.
  std::array<std::exception_ptr, 2> errors{};
  std::latch ready(2);
  std::array<std::jthread, 2> workers;
  for (std::size_t i = 0; i < 2; ++i) {
    workers[i] = std::jthread([&, i] {
      ready.arrive_and_wait();
      try {
        if (change(i, CompanionSocialAction::kAdd, 1 - i) !=
            CompanionSocialStatus::kOk) {
          throw std::runtime_error("Concurrent social change rejected.");
        }
      } catch (...) {
        errors[i] = std::current_exception();
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  for (const auto& error : errors) {
    if (error) {
      std::rethrow_exception(error);
    }
  }
  const auto first = snapshot(0);
  const auto second = snapshot(1);
  check(first.rows.size() == 1 && second.rows.size() == 1);
  const std::size_t recipient = first.rows[0].relationship == 2 ? 0 : 1;
  check(change(recipient, CompanionSocialAction::kAccept, 1 - recipient) ==
        CompanionSocialStatus::kOk);
  check(can(0, 1) == CompanionSocialStatus::kOk);
  // Rename changes display names, preserving friendship and block identities.
  MySqlConnection connection(configuration);
  {
    const std::array<std::uint8_t, 14> name{'R', 0,   'e', 0,   'n', 0,   'a',
                                            0,   'm', 0,   'e', 0,   'd', 0};
    Statement q(connection.Handle(),
                "UPDATE Characters SET NameUtf16 = ? WHERE CharacterId = ?");
    std::array parameters{
        Bytes(name.data(), static_cast<unsigned long>(name.size())),
        Number(players[1].character_id)};
    q.Execute(parameters);
  }
  check(snapshot(0).rows[0].name == u"Renamed" &&
        can(0, 1) == CompanionSocialStatus::kOk);
  check(!MySqlSocialStore(configuration).Find(target(1), u"SocialBob"));
  check(MySqlSocialStore(configuration).Find(0, u"Renamed")->character_id ==
        target(1));
  // Trigger exists only in generated test schema: a failed write grants no
  // block.
  rejects([&] { change(2, CompanionSocialAction::kBlock, 0); });
  check(snapshot(2).blocks.empty());
  // Disabled accounts cannot use an existing authenticated identity for social
  // changes.
  const auto disabled = [&](std::size_t player, std::uint8_t value) {
    Statement q(connection.Handle(),
                "UPDATE Accounts SET IsDisabled = ? WHERE AccountId = ?");
    std::array params{Number(value), Number(players[player].account_id)};
    q.Execute(params);
  };
  disabled(0, 1);
  rejects([&] { snapshot(0); });
  rejects([&] { can(0, 1); });
  rejects([&] { can(1, 0); });
  rejects([&] { change(0, CompanionSocialAction::kRemove, 1); });
  disabled(0, 0);
  check(snapshot(0).rows[0].relationship == 1 &&
        can(0, 1) == CompanionSocialStatus::kOk);
  std::cout << "Social persistence tests: " << checks << " checks passed.\n";
  return checks;
}
void SeedSocialNetworkTests(const std::filesystem::path& configuration) {
  using namespace persistence;
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  const auto password =
      features::authentication::CredentialCrypto::HashPassword(
          "Synthetic-network-password");
  const std::array users{"net_host", "net_guest", "net_observer"};
  const std::array names{u"RoomPeer", u"RoomProbe", u"RoomObserver"};
  for (std::size_t i = 0; i < users.size(); ++i) {
    if (!accounts.CreateAccount(users[i], password)) {
      throw std::runtime_error("Network account already exists.");
    }
    features::characters::CharacterRecord character{
        0, accounts.FindAccount(users[i])->account_id, names[i], 27, 2};
    if (characters.Create(character) !=
        features::characters::CharacterCreateResult::kCreated) {
      throw std::runtime_error("Network character creation failed.");
    }
  }
  std::cout << "3 isolated network accounts and characters created.\n";
}
}  // namespace bubble_fighter::server::tests
