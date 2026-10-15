#include <array>
#include <barrier>
#include <filesystem>
#include <future>
#include <iostream>
#include <source_location>

#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::tests {
void RunClanMasterTransferPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace features;
  using namespace protocol;
  int checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Clan transfer persistence line " +
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
  MySqlConnection connection(configuration);
  const auto sql = [&](const std::string& text) {
    my_sql_commands::Statement statement(connection.Handle(), text);
    statement.Execute({});
  };
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  const auto password = authentication::CredentialCrypto::HashPassword(
      "Synthetic-transfer-password");
  std::array<clans::ClanIdentity, 7> ids;
  const auto name = [](std::size_t i) {
    return u"Transfer" + std::u16string(1, static_cast<char16_t>(u'A' + i));
  };
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto username = "clan_transfer_" + std::to_string(i);
    check(accounts.CreateAccount(username, password));
    characters::CharacterRecord character{
        0, accounts.FindAccount(username)->account_id, name(i), 27, 2};
    check(characters.Create(character) ==
          characters::CharacterCreateResult::kCreated);
    ids[i] = {character.account_id,
              static_cast<std::uint32_t>(character.character_id)};
  }
  sql("INSERT INTO Clans(ClanId,NameUtf16,IntroductionUtf16,MemberCapacity,IsRecruiting,CreatedAtUnixSeconds) VALUES(7400,UNHEX('5400720061006E007300660065007200'),UNHEX('48006900'),30,1,1704067200),(7401,UNHEX('4F007400680065007200'),UNHEX('48006900'),30,1,1704067200)");
  const auto seed = [&](std::size_t i, int role, int clan = 7400) {
    sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES(" +
        std::to_string(ids[i].character_id) + "," + std::to_string(clan) + "," +
        std::to_string(role) + ")");
  };
  seed(0, 101);
  seed(1, 100);
  seed(2, 102);
  seed(3, 1);
  seed(4, 101, 7401);
  seed(5, 100, 7401);
  seed(6, 100);
  MySqlClanStore store(configuration);
  const auto role = [&](std::size_t i) {
    return store.Load(ids[i])->profile.membership_status;
  };
  const auto prepare = [&](std::size_t actor, std::size_t target) {
    return store.PrepareMasterTransfer(ids[actor], ids[target].character_id);
  };
  const auto commit = [&](const clans::ClanMasterTransferParties& parties) {
    return store.CommitMasterTransfer(parties, [] { return true; });
  };
  const auto refusal = [&](const clans::ClanMasterTransferParties& parties) {
    const auto result = commit(parties);
    check(result.responses.size() == 2 && result.deliveries.empty());
    check(ClanMasterTransferResponse::Decode(result.responses[0]).available ==
          0);
  };
  check(!prepare(1, 0) && !prepare(0, 0) && !prepare(0, 3));
  check(!prepare(0, 4) && !prepare(0, 5));
  check(!store.PrepareMasterTransfer({ids[1].account_id, ids[0].character_id},
                                     ids[1].character_id));
  const auto first = *prepare(0, 1);
  check(first.requester == ids[0] && first.target == ids[1] &&
        first.clan_id == 7400 && first.target_status == 100 &&
        first.requester_name == name(0) && first.target_name == name(1));
  check(role(0) == 101 && role(1) == 100);
  auto forged = first;
  forged.target_name = u"Forged";
  refusal(forged);
  forged = first;
  forged.requester_name = u"Forged";
  refusal(forged);
  forged = first;
  forged.target.account_id = ids[5].account_id;
  refusal(forged);
  forged = first;
  forged.target_status = 102;
  refusal(forged);
  forged = first;
  forged.clan_id = 7401;
  refusal(forged);
  rejects([&] { store.CommitMasterTransfer(first, {}); });
  rejects([&] { store.CommitMasterTransfer(first, [] { return false; }); });
  int decisions = 0;
  rejects([&] {
    store.CommitMasterTransfer(first, [&] { return ++decisions == 1; });
  });
  check(decisions == 2 && role(0) == 101 && role(1) == 100 && role(2) == 102);
  // A trigger fails the second write after the old master's demotion.
  const auto failing = *prepare(0, 6);
  rejects([&] { commit(failing); });
  check(role(0) == 101 && role(6) == 100);
  sql("UPDATE Accounts SET IsDisabled=1 WHERE AccountId=" +
      std::to_string(ids[1].account_id));
  check(!prepare(0, 1));
  refusal(first);
  sql("UPDATE Accounts SET IsDisabled=0 WHERE AccountId=" +
      std::to_string(ids[1].account_id));
  sql("UPDATE Clans SET DisbandAtUnixSeconds=1704153600 WHERE ClanId=7400");
  check(!prepare(0, 1));
  refusal(first);
  sql("UPDATE Clans SET DisbandAtUnixSeconds=0 WHERE ClanId=7400");
  sql("UPDATE ClanMembers SET MembershipStatus=1 WHERE CharacterId=" +
      std::to_string(ids[1].character_id));
  refusal(first);
  sql("UPDATE ClanMembers SET MembershipStatus=100 WHERE CharacterId=" +
      std::to_string(ids[1].character_id));
  const auto transferred = commit(first);
  const auto notice = ClanMasterTransferredNotification{
      0, ids[0].character_id, name(0), ids[1].character_id, name(1)};
  check(transferred.responses.size() == 3 &&
        transferred.deliveries.size() == 2);
  check(ClanMasterTransferredNotification::Decode(transferred.responses[0]) ==
        notice);
  check(
      ClanProfileResponse::Decode(transferred.responses[1]).membership_status ==
      100);
  check(ClanDetailsResponse::Decode(transferred.responses[2]).text_at76 ==
        name(1));
  check(transferred.deliveries[0].characters ==
        std::vector{ids[1].character_id});
  check(transferred.deliveries[0].packets->size() == 3 &&
        ClanProfileResponse::Decode(transferred.deliveries[0].packets->at(1))
                .membership_status == 101);
  check(transferred.deliveries[1].characters ==
        std::vector{ids[2].character_id, ids[6].character_id});
  check(role(0) == 100 && role(1) == 101 && role(2) == 102 && role(3) == 1);
  refusal(first);
  check(!prepare(0, 2));
  // The new authority persists after reopening and can transfer to a deputy.
  MySqlClanStore reopened(configuration);
  check(reopened.Load(ids[1])->profile.membership_status == 101);
  const auto deputy = *prepare(1, 2);
  check(deputy.target_status == 102);
  commit(deputy);
  check(role(1) == 100 && role(2) == 101 &&
        store.Load(ids[2])->details->text_at100.empty());
  commit(*prepare(2, 0));
  check(role(0) == 101 && role(2) == 100);
  // Independent stores compete for the same master; exactly one commits.
  const auto a = *prepare(0, 1), b = *prepare(0, 2);
  std::barrier gate(2);
  const auto attempt = [&](const auto& parties) {
    MySqlClanStore other(configuration);
    gate.arrive_and_wait();
    return other.CommitMasterTransfer(parties, [] { return true; });
  };
  auto left = std::async(std::launch::async, [&] { return attempt(a); });
  auto right = std::async(std::launch::async, [&] { return attempt(b); });
  const auto lm = left.get().responses[0].message_id;
  const auto rm = right.get().responses[0].message_id;
  check((lm == 1056 && rm == 1058) || (lm == 1058 && rm == 1056));
  check(role(0) == 100 && (role(1) == 101) != (role(2) == 101));
  sql("DELETE FROM Clans WHERE ClanId IN (7400,7401)");
  check(role(0) == 0 && role(4) == 0);
  std::cout << "Clan master transfer persistence: " << checks
            << " checks passed\n";
}
}  // namespace bubble_fighter::server::tests
