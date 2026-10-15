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
std::size_t RunFriendGroupPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace my_sql_commands;
  using namespace features;
  using namespace protocol;
  using Action = CompanionFriendGroupAction;
  using Status = CompanionFriendGroupStatus;
  std::size_t checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Friend group persistence line " +
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
      "Synthetic-group-password");
  std::array<characters::CharacterRecord, 3> players;
  const std::array users{"group_alice", "group_bob", "group_carol"};
  const std::array names{u"GroupAlice", u"GroupBob", u"GroupCarol"};
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
  const auto load = [&](std::size_t i) {
    return MySqlSocialStore(configuration).Load(players[i]);
  };
  const auto change = [&](std::size_t i, Action action, std::uint32_t group = 0,
                          std::uint32_t peer = 0, std::uint32_t source = 0,
                          std::u16string name = {}) {
    return MySqlSocialStore(configuration)
        .ChangeGroup(players[i],
                     {action, group, peer, source, std::move(name)});
  };
  const auto relationship = [&](std::size_t owner, CompanionSocialAction action,
                                std::size_t peer) {
    return MySqlSocialStore(configuration)
        .Change(players[owner], {action, id(peer), {}, {}})
        .status;
  };
  const auto create = [&](std::size_t owner, std::u16string name) {
    const auto result =
        change(owner, Action::kCreate, 0, 0, 0, std::move(name));
    check(result.status == Status::kOk);
    check(!result.snapshot.groups.empty());
    return result.snapshot.groups.back().group_id;
  };
  check(change(0, Action::kList).snapshot.groups.empty());
  const auto a = create(0, u"队友😀");
  const auto b = create(0, u"Raid");
  const auto other = create(1, u"队友😀");
  check(a != b && b != other);
  check(load(0).groups.size() == 2 && load(1).groups.size() == 1 &&
        load(2).groups.empty());
  check(change(0, Action::kCreate, 0, 0, 0, u"Raid").status ==
        Status::kDuplicate);
  check(change(0, Action::kRename, a, 0, 0, u"Raid").status ==
        Status::kDuplicate);
  check(change(0, Action::kRename, a, 0, 0, u"队友😀").status == Status::kOk);
  check(change(0, Action::kRename, other, 0, 0, u"stolen").status ==
        Status::kNotFound);
  check(change(0, Action::kRemove, other).status == Status::kNotFound);
  check(change(0, Action::kMove, a, id(1)).status == Status::kNotFriend);
  check(change(0, Action::kMove, a, id(0)).status == Status::kNotFriend);
  check(change(0, Action::kMove, a, UINT32_MAX).status == Status::kNotFriend);
  check(relationship(0, CompanionSocialAction::kAdd, 1) ==
        CompanionSocialStatus::kOk);
  check(change(0, Action::kMove, a, id(1)).status == Status::kNotFriend);
  check(relationship(1, CompanionSocialAction::kAccept, 0) ==
        CompanionSocialStatus::kOk);
  check(change(0, Action::kMove, other, id(1)).status == Status::kNotFound);
  auto moved = change(0, Action::kMove, a, id(1));
  check(moved.status == Status::kOk);
  check(moved.snapshot.assignments ==
        std::vector<CompanionFriendGroupAssignment>{{id(1), a}});
  check(load(1).assignments.empty());
  check(change(0, Action::kRemove, a).status == Status::kNotEmpty);
  check(change(0, Action::kMove, b, id(1), 0).status == Status::kStale);
  check(change(1, Action::kMove, other, id(0), 0).status == Status::kOk);
  check(relationship(0, CompanionSocialAction::kBlock, 1) ==
        CompanionSocialStatus::kOk);
  check(load(0).assignments == moved.snapshot.assignments &&
        load(0).rows[0].relationship == 4);
  check(change(0, Action::kMove, b, id(1), a).status == Status::kOk);
  check(change(0, Action::kMove, 0, id(1), b).status == Status::kOk);
  check(load(0).assignments.empty() && load(1).assignments.size() == 1);
  check(change(0, Action::kRemove, a).status == Status::kOk);
  const auto replacement = create(0, u"队友😀");
  check(replacement > a);  // Deleted identifiers are never recycled by the app.
  check(change(0, Action::kMove, a, id(1), 0).status == Status::kNotFound);
  check(change(0, Action::kMove, replacement, id(1), 0).status == Status::kOk);
  check(relationship(1, CompanionSocialAction::kRemove, 0) ==
        CompanionSocialStatus::kOk);
  check(load(0).assignments.empty() && load(1).assignments.empty());
  check(load(0).groups.size() == 2 && load(1).groups.size() == 1);
  check(load(0).rows[0].relationship ==
        4);  // Independent block survives friendship removal.
  check(relationship(0, CompanionSocialAction::kUnblock, 1) ==
        CompanionSocialStatus::kOk);
  check(relationship(0, CompanionSocialAction::kAdd, 1) ==
        CompanionSocialStatus::kOk);
  check(relationship(1, CompanionSocialAction::kAccept, 0) ==
        CompanionSocialStatus::kOk);
  check(change(0, Action::kMove, replacement, id(1)).status == Status::kOk);

  const auto rollback = create(0, u"Rollback");
  rejects([&] { change(0, Action::kMove, rollback, id(1), replacement); });
  check(load(0).assignments ==
        std::vector<CompanionFriendGroupAssignment>{{id(1), replacement}});
  check(change(0, Action::kMove, b, id(1), replacement).status == Status::kOk);
  rejects([&] { change(0, Action::kRename, b, 0, 0, u"Explode"); });
  check(load(0).groups[0].name == u"Raid");
  rejects(
      [&] { change(0, Action::kCreate, 0, 0, 0, std::u16string(32, u'G')); });
  rejects([&] { change(0, Action::kMove, b, id(1), b); });
  auto spoof = players[0];
  spoof.account_id = players[2].account_id;
  rejects([&] { MySqlSocialStore(configuration).ChangeGroup(spoof, {}); });
  rejects([&] {
    MySqlSocialStore(configuration)
        .ChangeGroup(spoof, {Action::kCreate, 0, 0, 0, u"forged"});
  });
  check(load(0).groups.size() == 3);

  MySqlConnection sql(configuration);
  const auto disabled = [&](std::size_t player, std::uint8_t value) {
    Statement q(sql.Handle(),
                "UPDATE Accounts SET IsDisabled = ? WHERE AccountId = ?");
    std::array parameters{Number(value), Number(players[player].account_id)};
    q.Execute(parameters);
  };
  disabled(0, 1);
  rejects([&] { change(0, Action::kList); });
  rejects([&] { change(0, Action::kRename, b, 0, 0, u"disabled"); });
  disabled(0, 0);
  disabled(1, 1);
  rejects([&] { change(0, Action::kMove, replacement, id(1), b); });
  disabled(1, 0);
  check(load(0).assignments ==
        std::vector<CompanionFriendGroupAssignment>{{id(1), b}});

  // Concurrent writers on the same owner cannot both consume the same source.
  const auto run_pair = [&](auto operation) {
    std::array<std::exception_ptr, 2> errors{};
    std::array<Status, 2> statuses{};
    std::latch ready(2);
    std::array<std::jthread, 2> workers;
    for (std::size_t i = 0; i < workers.size(); ++i) {
      workers[i] = std::jthread([&, i] {
        ready.arrive_and_wait();
        try {
          statuses[i] = operation(i);
        } catch (...) {
          errors[i] = std::current_exception();
        }
      });
    }
    for (auto& worker : workers) worker.join();
    for (const auto& error : errors)
      if (error) std::rethrow_exception(error);
    return statuses;
  };
  auto outcomes = run_pair([&](std::size_t) {
    return change(0, Action::kMove, replacement, id(1), b).status;
  });
  check((outcomes[0] == Status::kOk && outcomes[1] == Status::kStale) ||
        (outcomes[1] == Status::kOk && outcomes[0] == Status::kStale));
  outcomes = run_pair([&](std::size_t) {
    return change(2, Action::kCreate, 0, 0, 0, u"Same").status;
  });
  check((outcomes[0] == Status::kOk && outcomes[1] == Status::kDuplicate) ||
        (outcomes[1] == Status::kOk && outcomes[0] == Status::kDuplicate));
  for (std::uint32_t n = 0; n < 30; ++n)
    create(2, std::u16string(1, static_cast<char16_t>(0x4E00 + n)));
  outcomes = run_pair([&](std::size_t i) {
    return change(2, Action::kCreate, 0, 0, 0, i ? u"LastB" : u"LastA").status;
  });
  check((outcomes[0] == Status::kOk && outcomes[1] == Status::kCapacity) ||
        (outcomes[1] == Status::kOk && outcomes[0] == Status::kCapacity));
  check(load(2).groups.size() == 32);
  check(change(2, Action::kCreate, 0, 0, 0, u"Overflow").status ==
        Status::kCapacity);

  // SQL constraints independently reject cross-owner group and fake pair keys.
  const auto inject = [&](std::uint32_t group, std::uint32_t peer,
                          std::uint32_t first, std::uint32_t second) {
    Statement q(
        sql.Handle(),
        "INSERT INTO SocialFriendGroupAssignments (OwnerCharacterId, FriendCharacterId, GroupId, FirstCharacterId, SecondCharacterId) VALUES (?, ?, ?, ?, ?)");
    std::array parameters{Number(players[1].character_id), Number(peer),
                          Number(group), Number(first), Number(second)};
    q.Execute(parameters);
  };
  rejects([&] { inject(replacement, id(0), id(0), id(1)); });
  rejects([&] { inject(other, id(2), id(0), id(1)); });
  rejects([&] { inject(other, id(2), id(1), id(2)); });
  check(load(1).assignments.empty());
  // Direct external corruption of a valid name is rejected on read, not echoed.
  {
    Statement q(
        sql.Handle(),
        "UPDATE SocialFriendGroups SET NameUtf16 = X'00D8' WHERE GroupId = ?");
    std::array parameters{Number(other)};
    q.Execute(parameters);
  }
  rejects([&] { load(1); });
  {
    Statement q(
        sql.Handle(),
        "UPDATE SocialFriendGroups SET NameUtf16 = X'4200' WHERE GroupId = ?");
    std::array parameters{Number(other)};
    q.Execute(parameters);
  }
  check(load(1).groups[0].name == u"B");
  // Opposite owners move independently while taking the same ordered pair
  // locks.
  outcomes = run_pair([&](std::size_t i) {
    return i ? change(1, Action::kMove, other, id(0)).status
             : change(0, Action::kMove, b, id(1), replacement).status;
  });
  check(outcomes[0] == Status::kOk && outcomes[1] == Status::kOk);
  outcomes = run_pair([&](std::size_t i) {
    if (i) return change(0, Action::kMove, replacement, id(1), b).status;
    return relationship(0, CompanionSocialAction::kRemove, 1) ==
                   CompanionSocialStatus::kOk
               ? Status::kOk
               : Status::kInvalid;
  });
  check(outcomes[0] == Status::kOk &&
        (outcomes[1] == Status::kOk || outcomes[1] == Status::kNotFriend));
  check(load(0).assignments.empty() && load(1).assignments.empty());
  // Fresh store instances throughout verify persistence and lock/transaction
  // release.
  check(change(0, Action::kRemove, b).status == Status::kOk);
  check(change(0, Action::kRemove, b).status == Status::kNotFound);
  check(load(0).groups.size() == 2);
  std::cout << "Friend group persistence: " << checks << " checks passed.\n";
  return checks;
}
}  // namespace bubble_fighter::server::tests
