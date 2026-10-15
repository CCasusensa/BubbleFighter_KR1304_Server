#include <array>
#include <barrier>
#include <future>
#include <iostream>
#include <source_location>

#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::tests {
void RunClanTrainingPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace features;
  using namespace protocol;
  using Action = clans::TrainingClanAction;
  int checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Clan training persistence line " +
                               std::to_string(at.line()));
  };
  const auto rejects = [&](auto f) {
    bool failed = false;
    try {
      f();
    } catch (const std::exception&) {
      failed = true;
    }
    check(failed);
  };
  MySqlConnection connection(configuration);
  const auto sql = [&](const std::string& text) {
    my_sql_commands::Statement q(connection.Handle(), text);
    q.Execute({});
  };
  const auto scalar = [&](const std::string& text) {
    my_sql_commands::Statement q(connection.Handle(), text);
    q.Execute({});
    std::uint64_t value = 0;
    std::array fields{my_sql_commands::Number(value)};
    if (!q.Fetch(fields)) throw std::runtime_error("Missing aggregate");
    return value;
  };
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  const auto password = authentication::CredentialCrypto::HashPassword(
      "Synthetic-training-password");
  std::array<clans::ClanIdentity, 8> ids;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto username = "clan_training_" + std::to_string(i);
    check(accounts.CreateAccount(username, password));
    characters::CharacterRecord c{
        0, accounts.FindAccount(username)->account_id,
        u"Training" + std::u16string(1, static_cast<char16_t>(u'A' + i)), 27,
        2};
    check(characters.Create(c) == characters::CharacterCreateResult::kCreated);
    ids[i] = {c.account_id, static_cast<std::uint32_t>(c.character_id)};
  }
  const auto cid = [&](int i) { return std::to_string(ids[i].character_id); };
  MySqlClanStore store(configuration);
  const auto act = [&](int i, Action action, std::uint64_t now = 1704067300) {
    return store.Training(ids[i], action, now, [] { return true; });
  };
  const auto joined = [&](const auto& result) {
    check(!result.mutation.responses.empty());
    return TrainingClanJoinResponse::Decode(result.mutation.responses[0])
               .clan_id_or_failure == 99;
  };
  const auto member = [&](int i) {
    return store.Load(ids[i])->profile.clan_id;
  };
  sql("UPDATE Characters SET Level=45 WHERE CharacterId=" + cid(0));
  sql("UPDATE Characters SET Level=46 WHERE CharacterId=" + cid(1));
  check(!joined(act(1, Action::kJoin)) && member(1) == 0);
  check(scalar("SELECT COUNT(*) FROM Clans WHERE ClanId=99") == 0);
  sql("INSERT INTO ClanDisbandNotices VALUES(" + cid(0) +
      ",8000,X'47006F006E006500',1704067200)");
  check(joined(act(0, Action::kJoin)));
  check(member(0) == 99 && !store.Load(ids[0])->disband_notice);
  const auto details = *store.Load(ids[0])->details;
  check(details.mark_id == 49 && details.text_at76.empty() &&
        details.text_at100.empty());
  check(details.value_at132 == 1 && details.value_at136 == 0 &&
        details.value_at164 == 1);
  check(!joined(act(0, Action::kJoin)));
  check(!act(0, Action::kGraduate).graduated && member(0) == 99);
  check(!act(0, Action::kUnsupported).graduated && member(0) == 99);
  check(act(0, Action::kReconcile).mutation.responses.empty());
  check(store.Search(ids[0], {6, 1, {}})->clans.empty());
  for (auto kind :
       {clans::ClanRosterKind::kMembers, clans::ClanRosterKind::kDetails,
        clans::ClanRosterKind::kApplicants})
    check(store.ReadRoster(ids[0], kind)->at(0).message_id == 1016);
  check(
      ClanDisbandResponse::Decode(
          store
              .Disband(ids[0], {1}, 1704067300, 1704067400, [] { return true; })
              .responses[0])
          .result == 0);
  rejects([&] {
    sql("UPDATE ClanMembers SET MembershipStatus=101 WHERE CharacterId=" +
        cid(0));
  });
  rejects([&] {
    store.Training({ids[1].account_id, ids[0].character_id}, Action::kJoin,
                   1704067300, [] { return true; });
  });
  for (auto now : {0ULL, 4102444800ULL})
    rejects([&] { act(0, Action::kManualLeave, now); });
  sql("UPDATE Accounts SET IsDisabled=1 WHERE Username='clan_training_4'");
  rejects([&] { act(4, Action::kJoin); });
  sql("UPDATE Accounts SET IsDisabled=0 WHERE Username='clan_training_4'");
  int calls = 0;
  rejects([&] {
    store.Training(ids[4], Action::kJoin, 1704067300,
                   [&] { return ++calls == 1; });
  });
  check(member(4) == 0 &&
        scalar("SELECT COUNT(*) FROM TrainingClanHistory WHERE CharacterId=" +
               cid(4)) == 0);
  rejects([&] {
    act(5, Action::kJoin);
  });  // Last history write fails after member INSERT.
  check(member(5) == 0 &&
        scalar("SELECT COUNT(*) FROM TrainingClanHistory WHERE CharacterId=" +
               cid(5)) == 0);
  auto left = act(0, Action::kManualLeave, 1704067301);
  check(left.handled && !left.graduated && left.mutation.responses.size() == 2);
  check(ClanLeaveNotification::Decode(left.mutation.responses[0]).new_status ==
        0xffffffecU);
  check(member(0) == 0);
  check(!act(0, Action::kManualLeave, 1704067302).handled);
  const auto reopened = MySqlClanStore(configuration).Load(ids[0]);
  check(reopened->training_exit_notice &&
        reopened->training_exit_notice->membership_status == 0xffffffecU);
  check(!joined(act(0, Action::kJoin, 1704067300)));
  check(!joined(act(0, Action::kJoin, 1704067360)));
  check(joined(act(0, Action::kJoin, 1704067361)));
  check(!store.Load(ids[0])->training_exit_notice);
  sql("UPDATE Characters SET Level=46 WHERE CharacterId=" + cid(0));
  auto graduated = act(0, Action::kReconcile, 1704067362);
  check(graduated.graduated && member(0) == 0);
  check(ClanLeaveNotification::Decode(graduated.mutation.responses[0])
            .new_status == 0xffffffc4U);
  check(act(0, Action::kReconcile, 1704067363).mutation.responses.empty());
  check(act(0, Action::kGraduate, 1704067363).graduated);
  sql("UPDATE Characters SET Level=1 WHERE CharacterId=" + cid(0));
  check(
      !joined(act(0, Action::kJoin,
                  1704068000)));  // Durable graduation cannot be replayed away.
  check(MySqlClanStore(configuration)
            .Load(ids[0])
            ->training_exit_notice->membership_status == 0xffffffc4U);
  check(joined(act(6, Action::kJoin)));
  rejects([&] { act(6, Action::kManualLeave, 1704067301); });
  check(member(6) == 99);
  sql("UPDATE Characters SET Level=46 WHERE CharacterId=" + cid(6));
  rejects([&] { act(6, Action::kReconcile, 1704067302); });
  check(member(6) == 99 &&
        scalar("SELECT Graduated FROM TrainingClanHistory WHERE CharacterId=" +
               cid(6)) == 0);
  std::barrier gate(2);
  const auto racing = [&] {
    MySqlClanStore concurrent(configuration);
    gate.arrive_and_wait();
    return concurrent.Training(ids[2], Action::kJoin, 1704067300,
                               [] { return true; });
  };
  auto a = std::async(std::launch::async, racing),
       b = std::async(std::launch::async, racing);
  check(static_cast<int>(joined(a.get())) + static_cast<int>(joined(b.get())) ==
        1);
  check(member(2) == 99 &&
        scalar("SELECT COUNT(*) FROM TrainingClanHistory WHERE CharacterId=" +
               cid(2)) == 1);
  sql("INSERT INTO Clans(ClanId,NameUtf16,IntroductionUtf16,MemberCapacity,IsRecruiting,CreatedAtUnixSeconds) VALUES(7600,X'470072006100640065007300',X'48006900',30,2,1704067200)");
  sql("INSERT INTO ClanMembers VALUES(" + cid(7) + ",7600,101,DEFAULT),(" +
      cid(3) + ",7600,1,DEFAULT)");
  check(!joined(act(3, Action::kJoin)) && member(3) == 7600);
  check(!act(3, Action::kManualLeave).handled);
  auto ordinary =
      store.Mutate(ids[0], ClanAutoJoinRequest{7600, u"Grades", u"TrainingA"},
                   [] { return true; });
  check(ClanJoinResponse::Decode(ordinary.responses[0]).result == 1);
  check(member(0) == 7600 && !store.Load(ids[0])->training_exit_notice);
  check(scalar("SELECT ExitStatus FROM TrainingClanHistory WHERE CharacterId=" +
               cid(0)) == 0);
  store.Mutate(ids[0], ClanLeaveRequest{}, [] { return true; });
  check(member(0) == 0 && !store.Load(ids[0])->training_exit_notice);
  check(act(0, Action::kReconcile, 1704068000).graduated);
  std::barrier mutual(2);
  auto train = std::async(std::launch::async, [&] {
    MySqlClanStore concurrent(configuration);
    mutual.arrive_and_wait();
    return concurrent.Training(ids[4], Action::kJoin, 1704068000,
                               [] { return true; });
  });
  auto normal = std::async(std::launch::async, [&] {
    MySqlClanStore concurrent(configuration);
    mutual.arrive_and_wait();
    return concurrent.Mutate(ids[4],
                             ClanAutoJoinRequest{7600, u"Grades", u"TrainingE"},
                             [] { return true; });
  });
  const bool training_won = joined(train.get());
  const bool ordinary_won =
      ClanJoinResponse::Decode(normal.get().responses[0]).result == 1;
  check(training_won != ordinary_won);
  check(member(4) == (training_won ? 99U : 7600U));
  check(scalar("SELECT COUNT(*) FROM ClanMembers WHERE CharacterId=" +
               cid(4)) == 1);
  sql("DELETE FROM Clans WHERE ClanId IN (99,7600)");
  std::cout << "Clan training persistence: " << checks << " checks passed\n";
}
}  // namespace bubble_fighter::server::tests
