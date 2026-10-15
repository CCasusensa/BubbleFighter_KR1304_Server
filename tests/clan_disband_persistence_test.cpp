#include <array>
#include <barrier>
#include <future>
#include <iostream>
#include <source_location>

#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_inventory_store.h"

namespace bubble_fighter::server::tests {
void RunClanDisbandPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace features;
  using namespace protocol;
  int checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Clan disband persistence line " +
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
  MySqlInventoryStore inventory(configuration);
  const auto password = authentication::CredentialCrypto::HashPassword(
      "Synthetic-disband-password");
  std::array<clans::ClanIdentity, 7> ids;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto username = "clan_disband_" + std::to_string(i);
    check(accounts.CreateAccount(username, password));
    characters::CharacterRecord c{
        0, accounts.FindAccount(username)->account_id,
        u"Disband" + std::u16string(1, static_cast<char16_t>(u'A' + i)), 27, 2};
    check(characters.Create(c) == characters::CharacterCreateResult::kCreated);
    ids[i] = {c.account_id, static_cast<std::uint32_t>(c.character_id)};
  }
  for (int clan = 7500; clan <= 7503; ++clan) {
    sql("INSERT INTO Clans(ClanId,NameUtf16,IntroductionUtf16,MemberCapacity,IsRecruiting,CreatedAtUnixSeconds) VALUES(" +
        std::to_string(clan) + ",UNHEX('47006F006E006500" +
        std::to_string(41 + clan - 7500) +
        "00'),UNHEX('48006900'),30,1,1704067200)");
  }
  const auto seed = [&](std::size_t i, int role, int clan) {
    sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES(" +
        std::to_string(ids[i].character_id) + "," + std::to_string(clan) + "," +
        std::to_string(role) + ")");
  };
  seed(0, 101, 7500);
  seed(1, 100, 7500);
  seed(2, 102, 7500);
  seed(3, 1, 7500);
  seed(4, 101, 7501);
  seed(5, 101, 7502);
  seed(6, 101, 7503);
  MySqlClanStore store(configuration);
  constexpr std::uint64_t now = 1704067300, due = now + 100;
  const auto act = [&](std::size_t actor, std::uint32_t action,
                       std::uint64_t at = 1704067300,
                       std::uint64_t next = 1704067400) {
    return store.Disband(ids[actor], {action}, at, next, [] { return true; });
  };
  const auto success = [&](const auto& result) {
    check(!result.responses.empty());
    return ClanDisbandResponse::Decode(result.responses[0]).result == 1;
  };
  for (const auto i : {1, 2, 3}) check(!success(act(i, 1)));
  for (const auto action : {0U, 3U, 0xffffffffU})
    check(!success(act(0, action)));
  check(!success(act(0, 2)));
  for (const auto next : {std::uint64_t{0}, now, now - 1, 4102444800ULL})
    check(!success(act(0, 1, now, next)));
  rejects([&] {
    store.Disband({ids[1].account_id, ids[0].character_id}, {1}, now, due,
                  [] { return true; });
  });
  rejects([&] { store.Disband(ids[0], {1}, now, due, {}); });
  sql("UPDATE Accounts SET IsDisabled=1 WHERE Username='clan_disband_0'");
  rejects([&] { act(0, 1); });
  sql("UPDATE Accounts SET IsDisabled=0 WHERE Username='clan_disband_0'");
  int calls = 0;
  rejects([&] {
    store.Disband(ids[0], {1}, now, due, [&] { return ++calls == 1; });
  });
  check(scalar("SELECT DisbandAtUnixSeconds FROM Clans WHERE ClanId=7500") ==
        0);
  rejects([&] { act(6, 1); });
  check(scalar("SELECT DisbandAtUnixSeconds FROM Clans WHERE ClanId=7503") ==
        0);
  auto result = act(0, 1);
  check(success(result) && result.responses.size() == 3 &&
        result.deliveries.size() == 2);
  check(result.deliveries[0].characters ==
        std::vector<std::uint32_t>({ids[1].character_id, ids[2].character_id}));
  check(result.deliveries[1].characters ==
        std::vector<std::uint32_t>({ids[3].character_id}));
  check(ClanProfileResponse::Decode(result.deliveries[1].packets->front())
            .membership_status == 1);
  check(!success(act(0, 1, now, due + 1000)));
  check(scalar("SELECT DisbandAtUnixSeconds FROM Clans WHERE ClanId=7500") ==
        due);
  check(!store.PrepareMasterTransfer(ids[0], ids[1].character_id));
  check(MySqlClanStore(configuration).Load(ids[0])->profile.disband_date_bits ==
        ClanProfileResponse::Decode(result.responses[2]).disband_date_bits);
  check(success(act(0, 2, now, 0)));
  check(!success(act(0, 2)));
  std::barrier gate(2);
  const auto race = [&] {
    MySqlClanStore contender(configuration);
    gate.arrive_and_wait();
    return ClanDisbandResponse::Decode(
               contender.Disband(ids[0], {1}, now, due, [] { return true; })
                   .responses[0])
        .result;
  };
  auto first = std::async(std::launch::async, race),
       second = std::async(std::launch::async, race);
  check(first.get() + second.get() == 1);
  check(!success(act(0, 2, due)));
  check(store.DisbandDue(due - 1) == 0);
  check(success(act(5, 1)));
  rejects([&] {
    store.DisbandDue(due);
  });  // Second clan delete fails after first was staged.
  check(scalar("SELECT COUNT(*) FROM Clans WHERE ClanId IN (7500,7502)") == 2);
  check(scalar("SELECT COUNT(*) FROM ClanDisbandNotices") == 0);
  check(success(act(5, 2)));
  sql("INSERT INTO ClanCreationReceipts(ClanId,CharacterId,ItemId,InventoryRevision,CreatedAtUnixSeconds) VALUES(7500," +
      std::to_string(ids[0].character_id) + ",100819,1,1704067200)");
  check(store.DisbandDue(due) == 1);
  check(store.DisbandDue(due) == 0);
  for (std::size_t i = 0; i < 4; ++i) {
    const auto s = MySqlClanStore(configuration).Load(ids[i]);
    check(s && s->profile.clan_id == 0 && s->profile.membership_status == 0 &&
          !s->details);
    check(s->disband_notice &&
          s->disband_notice->membership_status == 0xffffffd8U &&
          s->disband_notice->clan_id == 7500 &&
          s->disband_notice->clan_name == u"GoneA");
  }
  check(scalar("SELECT COUNT(*) FROM ClanCreationReceipts WHERE ClanId=7500") ==
        1);
  check(scalar("SELECT COUNT(*) FROM ClanMembers WHERE ClanId=7500") == 0);
  check(
      scalar("SELECT COUNT(*) FROM Clans WHERE ClanId BETWEEN 7501 AND 7503") ==
      3);
  auto joined = store.Mutate(ids[1], ClanJoinRequest{7501, u"GoneB"},
                             [] { return true; });
  check(ClanJoinResponse::Decode(joined.responses[0]).result == 1);
  check(!store.Load(ids[1])->disband_notice);
  store.Mutate(ids[1], ClanCancelJoinRequest{7501}, [] { return true; });
  check(!store.Load(ids[1])->disband_notice &&
        store.Load(ids[1])->profile.membership_status == 0);
  inventory::InventorySlot card{100819, inventory::InventoryUsage::kCount, 2};
  check(inventory.Save(ids[0].account_id, ids[0].character_id, card, now));
  auto created = store.Create(ids[0], {u"Reborn", 0, u"Hi", 100819, 30},
                              due + 1, [] { return true; });
  check(created.result == 1 && !store.Load(ids[0])->disband_notice);
  check(scalar("SELECT COUNT(*) FROM ClanCreationReceipts WHERE ClanId=7500") ==
        1);
  {
    MySqlClanStore guard(configuration);
    guard.LockService();
    rejects([&] { MySqlClanStore(configuration).LockService(); });
  }
  MySqlClanStore(configuration).LockService();
  check(true);
  rejects([&] { store.DisbandDue(0); });
  rejects([&] { store.DisbandDue(4102444800ULL); });
  // Remove only these isolated test entities; failure-injected clan is kept
  // inert.
  sql("DELETE FROM ClanMembers WHERE ClanId BETWEEN 7500 AND 7503");
  sql("DELETE FROM Clans WHERE ClanId IN (7501,7503)");
  sql("DELETE FROM Clans WHERE NameUtf16=UNHEX('5200650062006F0072006E00')");
  sql("UPDATE Clans SET ClanId=7504 WHERE ClanId=7502");
  sql("DELETE FROM Clans WHERE ClanId=7504");
  std::cout << "Clan disband persistence: " << checks << " checks passed\n";
}
}  // namespace bubble_fighter::server::tests
