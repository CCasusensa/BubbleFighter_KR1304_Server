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
void RunClanMutationPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace features;
  using namespace protocol;
  int checks = 0;
  const auto check = [&](bool value, std::source_location at =
                                         std::source_location::current()) {
    ++checks;
    if (!value)
      throw std::runtime_error("Clan mutation persistence line " +
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
    my_sql_commands::Statement q(connection.Handle(), text);
    q.Execute({});
  };
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  const auto password = authentication::CredentialCrypto::HashPassword(
      "Synthetic-mutation-password");
  std::array<clans::ClanIdentity, 12> ids;
  const auto name = [](std::size_t i) {
    return u"Mutation" + std::u16string(1, static_cast<char16_t>(u'A' + i));
  };
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto username = "clan_mutation_" + std::to_string(i);
    check(accounts.CreateAccount(username, password));
    characters::CharacterRecord c{0, accounts.FindAccount(username)->account_id,
                                  name(i), 27, 2};
    check(characters.Create(c) == characters::CharacterCreateResult::kCreated);
    ids[i] = {c.account_id, static_cast<std::uint32_t>(c.character_id)};
  }
  sql("INSERT INTO Clans(ClanId,NameUtf16,IntroductionUtf16,MemberCapacity,IsRecruiting,CreatedAtUnixSeconds) VALUES(7300,UNHEX('41006C00700068006100'),UNHEX('48006900'),3,1,1704067200),(7301,UNHEX('4200650074006100'),UNHEX('48006900'),30,1,1704067200)");
  const auto seed = [&](std::size_t i, int role, int clan = 7300) {
    sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES(" +
        std::to_string(ids[i].character_id) + "," + std::to_string(clan) + "," +
        std::to_string(role) + ")");
  };
  seed(0, 101);
  seed(5, 101, 7301);
  MySqlClanStore store(configuration);
  const auto run = [&](std::size_t i, const clans::ClanMutation& request) {
    return store.Mutate(ids[i], request, [] { return true; });
  };
  const auto role = [&](std::size_t i) {
    return store.Load(ids[i])->profile.membership_status;
  };
  const auto mode = [&] { return store.Load(ids[0])->details->recruiting; };
  const auto recruit = [&](std::size_t actor, std::uint32_t value,
                           std::uint32_t clan = 7300) {
    return run(actor, ClanRecruitmentRequest{clan, value});
  };
  const auto join = [&](std::size_t i) {
    return run(i, ClanJoinRequest{7300, u"Alpha"});
  };
  const auto automatic = [&](std::size_t i) {
    return run(i, ClanAutoJoinRequest{7300, u"Alpha", name(i)});
  };
  const auto change = [&](std::size_t actor, std::size_t target,
                          std::uint32_t before, std::uint32_t after) {
    return run(actor, ClanMemberChangeRequest{ids[target].character_id,
                                              name(target), before, after});
  };
  const auto changed = [](const auto& r) {
    return ClanMemberChangeResponse::Decode(r.responses.at(0)).result;
  };
  const auto joined = [](const auto& r) {
    return ClanJoinResponse::Decode(r.responses.at(0));
  };
  const auto accepted = [](const auto& r) {
    return ClanRecruitmentResponse::Decode(r.responses.at(0)).result;
  };
  check(accepted(recruit(1, 2)) == 0 && mode() == 1);
  check(accepted(recruit(0, 3)) == 0 && accepted(recruit(0, 2, 7301)) == 0 &&
        mode() == 1);
  check(accepted(recruit(0, 2)) == 1 && mode() == 2);
  check(accepted(recruit(0, 2)) == 1 && mode() == 2);
  auto directory = store.Search(ids[1], {4, 1, u"Alpha"});
  check(directory->clans[0].recruitment_mode == 2);
  check(joined(run(1, ClanAutoJoinRequest{7300, u"Alpha", u"Forged"})).result ==
            0 &&
        role(1) == 0);
  auto entered = automatic(1);
  check(joined(entered).result == 1 &&
        joined(entered).membership_status == 100 && role(1) == 100);
  check(entered.responses.size() == 2 &&
        ClanProfileResponse::Decode(entered.responses[1]).clan_id == 7300);
  check(entered.deliveries.size() == 1 &&
        entered.deliveries[0].characters == std::vector{ids[0].character_id});
  check(entered.deliveries[0].packets->at(0).message_id == 1018 &&
        ClanRosterResponse::Decode(entered.deliveries[0].packets->at(1))
                .members.size() == 2);
  check(changed(change(0, 1, 100, 102)) == 1 && role(1) == 102);
  check(accepted(recruit(1, 1)) == 1 && mode() == 1);
  check(joined(automatic(2)).result == 0xfffffffa && role(2) == 0);
  auto pending = join(2);
  check(joined(pending).result == 1 && role(2) == 1);
  check(pending.deliveries[0].characters ==
            std::vector{ids[0].character_id, ids[1].character_id} &&
        pending.deliveries[0].packets->at(0).message_id == 1035);
  auto duplicate = joined(join(2));
  check(duplicate.result == 0xfffffffe && duplicate.membership_status == 1 &&
        duplicate.clan_id == 7300);
  auto approval = change(1, 2, 1, 100);
  check(changed(approval) == 1 && role(2) == 100);
  const auto& target = *approval.deliveries[0].packets;
  check(target.size() == 3 &&
        ClanProfileResponse::Decode(target[0]).membership_status == 1 &&
        ClanProfileResponse::Decode(target[0]).clan_id == 7300);
  check(ClanMemberChangedNotification::Decode(target[1]).new_status == 100 &&
        ClanMemberChangedNotification::Decode(target[1]).old_status == 1);
  check(ClanProfileResponse::Decode(target[2]).membership_status == 100);
  check(changed(change(0, 2, 100, 102)) == 0xffffff9a && role(2) == 100);
  check(changed(change(1, 2, 100, 102)) == 0 &&
        changed(change(0, 2, 100, 101)) == 0);
  check(changed(change(1, 1, 102, 100)) == 0 &&
        changed(change(2, 1, 102, 100)) == 0);
  check(changed(change(0, 2, 1, 0xffffffe2)) == 0 && role(2) == 100);
  check(changed(run(0, ClanMemberChangeRequest{ids[2].character_id, u"Wrong",
                                               100, 0xffffffe2})) == 0);
  auto foreign = run(0, ClanMemberChangeRequest{ids[5].character_id, name(5),
                                                101, 0xffffffe2});
  check(changed(foreign) == 0 &&
        ClanMemberChangeResponse::Decode(foreign.responses[0])
            .character_name.empty());
  check(joined(join(3)).result == 1 && role(3) == 1);
  auto full = change(0, 3, 1, 100);
  check(changed(full) == 2 && role(3) == 1 && full.deliveries.empty());
  check(changed(change(1, 0, 101, 0xffffffe2)) == 0 && role(0) == 101);
  check(changed(change(1, 2, 100, 0xffffffe2)) == 1 && role(2) == 0);
  check(ClanCancelJoinResponse::Decode(
            run(3, ClanCancelJoinRequest{7301}).responses[0])
                .result == 0 &&
        role(3) == 1);
  auto cancelled = run(3, ClanCancelJoinRequest{7300});
  check(ClanCancelJoinResponse::Decode(cancelled.responses[0]).result == 1 &&
        role(3) == 0);
  check(cancelled.deliveries[0].packets->at(0).message_id == 1065 &&
        ClanProfileResponse::Decode(cancelled.responses[1]).membership_status ==
            0);
  check(ClanCancelJoinResponse::Decode(
            run(3, ClanCancelJoinRequest{7300}).responses[0])
            .result == 0);
  check(joined(join(3)).result == 1 && joined(join(4)).result == 1);
  auto stopped = recruit(0, 0);
  check(accepted(stopped) == 1 && mode() == 0);
  check(stopped.deliveries.size() == 2 &&
        stopped.deliveries[0].packets->at(0).message_id == 1018 &&
        stopped.deliveries[1].packets->at(0).message_id == 1035);
  auto list =
      store.ReadRoster(ids[0], clans::ClanRosterKind::kApplicants).value();
  check(ClanDetailsResponse::Decode(list[0]).recruiting == 0 &&
        ClanApplicantsResponse::Decode(list[1]).applicants.size() == 2);
  check(changed(change(0, 3, 1, 100)) == 0 &&
        joined(join(6)).result == 0xfffffffb);
  check(changed(change(0, 3, 1, 0xfffffff6)) == 1 && role(3) == 0);
  check(changed(change(1, 4, 1, 0xfffffff6)) == 1 && role(4) == 0);
  check(changed(change(0, 3, 1, 0xfffffff6)) == 0);
  auto left = run(1, ClanLeaveRequest{});
  check(ClanLeaveNotification::Decode(left.responses[0]).old_status == 102 &&
        ClanLeaveNotification::Decode(left.responses[0]).new_status ==
            0xffffffec &&
        role(1) == 0);
  check(ClanProfileResponse::Decode(left.responses[1]).membership_status == 0 &&
        left.deliveries[0].packets->at(0).message_id == 1037);
  check(ClanProfileResponse::Decode(run(0, ClanLeaveRequest{}).responses[0])
                .membership_status == 101 &&
        role(0) == 101);
  check(accepted(recruit(0, 1)) == 1);
  check(joined(join(9)).result == 1);
  rejects([&] { change(0, 9, 1, 100); });
  check(role(9) == 1);
  check(changed(change(0, 9, 1, 0xfffffff6)) == 1 && role(9) == 0);
  int decisions = 0;
  rejects([&] {
    store.Mutate(ids[0], ClanRecruitmentRequest{7300, 2},
                 [&] { return ++decisions == 1; });
  });
  check(decisions == 2 && mode() == 1);
  decisions = 0;
  rejects([&] {
    store.Mutate(ids[6], ClanJoinRequest{7300, u"Alpha"},
                 [&] { return ++decisions == 1; });
  });
  check(decisions == 2 && role(6) == 0);
  rejects([&] {
    store.Mutate(ids[0], ClanRecruitmentRequest{7300, 0}, [] { return false; });
  });
  rejects([&] {
    store.Mutate({ids[1].account_id, ids[0].character_id},
                 ClanRecruitmentRequest{7300, 0}, [] { return true; });
  });
  rejects([&] { recruit(5, 2, 7301); });
  check(store.Load(ids[5])->details->recruiting == 1);
  sql("UPDATE Accounts SET IsDisabled=1 WHERE Username='clan_mutation_0'");
  rejects([&] { recruit(0, 0); });
  sql("UPDATE Accounts SET IsDisabled=0 WHERE Username='clan_mutation_0'");
  check(mode() == 1);
  seed(1, 102);
  seed(3, 1);
  seed(4, 1);
  // Different officers, different targets, one free slot: the shared clan row
  // lock protects the count even when these direct stores bypass session locks.
  std::barrier gate(2);
  auto concurrent = [&](std::size_t actor, std::size_t target_index) {
    MySqlClanStore other(configuration);
    gate.arrive_and_wait();
    return other.Mutate(ids[actor],
                        ClanMemberChangeRequest{ids[target_index].character_id,
                                                name(target_index), 1, 100},
                        [] { return true; });
  };
  auto first = std::async(std::launch::async, [&] { return concurrent(0, 3); });
  auto second =
      std::async(std::launch::async, [&] { return concurrent(1, 4); });
  const auto a = changed(first.get()), b = changed(second.get());
  check((a == 1 && b == 2) || (a == 2 && b == 1));
  check(store.Load(ids[0])->details->value_at164 == 3 &&
        (role(3) == 100) != (role(4) == 100));
  std::barrier other_gate(2);
  auto competing = [&](std::uint32_t id, std::u16string clan) {
    MySqlClanStore other(configuration);
    other_gate.arrive_and_wait();
    return other.Mutate(ids[8], ClanJoinRequest{id, clan}, [] { return true; });
  };
  auto alpha =
      std::async(std::launch::async, [&] { return competing(7300, u"Alpha"); });
  auto beta =
      std::async(std::launch::async, [&] { return competing(7301, u"Beta"); });
  const auto ar = joined(alpha.get()), br = joined(beta.get());
  check((ar.result == 1 && br.result == 0xfffffffe) ||
        (ar.result == 0xfffffffe && br.result == 1));
  check(ar.membership_status == 1 && br.membership_status == 1 &&
        ar.clan_id == br.clan_id);
  sql("DELETE FROM ClanMembers WHERE ClanId=7300 AND MembershipStatus<>101");
  sql("DELETE FROM ClanMembers WHERE CharacterId=" +
      std::to_string(ids[8].character_id));
  seed(1, 100);
  seed(3, 1);
  sql("UPDATE Clans SET DisbandAtUnixSeconds=1704153600 WHERE ClanId=7300");
  check(accepted(recruit(0, 2)) == 0 && joined(join(6)).result == 0xfffffffd);
  check(changed(change(0, 1, 100, 102)) == 0 &&
        changed(change(0, 3, 1, 100)) == 0);
  check(changed(change(0, 3, 1, 0xfffffff6)) == 1);
  check(ClanLeaveNotification::Decode(run(1, ClanLeaveRequest{}).responses[0])
            .new_status == 0xffffffec);
  sql("UPDATE Clans SET DisbandAtUnixSeconds=0 WHERE ClanId=7300");
  // Independent 500-applicant limit fixture, using disabled synthetic accounts.
  std::string av, cv, mv;
  constexpr char digits[] = "0123456789ABCDEF";
  for (int i = 0; i < 500; ++i) {
    const auto id = std::to_string(2500000 + i);
    const auto suffix = std::to_string(i);
    const auto n = std::string("MutationBulk") +
                   std::string(4 - suffix.size(), '0') + suffix;
    std::string hex;
    for (unsigned char c : n) {
      hex += digits[c >> 4];
      hex += digits[c & 15];
      hex += "00";
    }
    if (i) {
      av += ',';
      cv += ',';
      mv += ',';
    }
    av += "(" + id + ",'mutation_bulk_" + suffix +
          "',UNHEX(REPEAT('00',16)),UNHEX(REPEAT('00',32)),600000,1)";
    cv += "(" + id + "," + id + ",UNHEX('" + hex + "'),27,2)";
    mv += "(" + id + ",7300,1)";
  }
  sql("INSERT INTO Accounts(AccountId,Username,PasswordSalt,PasswordHash,PasswordIterations,IsDisabled) VALUES" +
      av);
  sql("INSERT INTO Characters(CharacterId,AccountId,NameUtf16,CharacterType,ColorIndex) VALUES" +
      cv);
  sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES" +
      mv);
  auto capped = join(7);
  check(joined(capped).result == 0xffffffff && role(7) == 0 &&
        capped.deliveries.empty());
  check(ClanApplicantsResponse::Decode(
            store.ReadRoster(ids[0], clans::ClanRosterKind::kApplicants)->at(1))
            .applicants.size() == 500);
  sql("DELETE FROM Clans WHERE ClanId IN (7300,7301)");
  sql("DELETE FROM Characters WHERE CharacterId BETWEEN 2500000 AND 2500499");
  sql("DELETE FROM Accounts WHERE AccountId BETWEEN 2500000 AND 2500499");
  check(role(0) == 0 && role(5) == 0);
  std::cout << "Clan mutation persistence: " << checks << " checks passed\n";
}
}  // namespace bubble_fighter::server::tests
