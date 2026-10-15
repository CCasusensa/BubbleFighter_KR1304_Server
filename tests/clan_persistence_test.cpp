#include <array>
#include <bit>
#include <iostream>
#include <source_location>

#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::tests {
void RunClanPersistenceTests(const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace features;
  int checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Clan persistence line " +
                               std::to_string(at.line()));
  };
  const auto rejects = [&](auto action) {
    bool rejected = false;
    try {
      action();
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected);
  };
  MySqlConnection connection(configuration);
  const auto sql = [&](const std::string& statement) {
    // All statements below contain only fixed fixtures or generated numeric
    // IDs.
    my_sql_commands::Statement query(connection.Handle(), statement);
    query.Execute({});
  };
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  const auto password =
      authentication::CredentialCrypto::HashPassword("Synthetic-clan-password");
  std::array<clans::ClanIdentity, 4> identities;
  const std::array names{u"ClanMaster", u"ClanDeputy", u"ClanMember",
                         u"ClanApplicant"};
  for (std::size_t i = 0; i < identities.size(); ++i) {
    const auto username = "clan_reader_" + std::to_string(i);
    check(accounts.CreateAccount(username, password));
    characters::CharacterRecord c{0, accounts.FindAccount(username)->account_id,
                                  names[i], 27, 2};
    check(characters.Create(c) == characters::CharacterCreateResult::kCreated);
    identities[i] = {c.account_id, static_cast<std::uint32_t>(c.character_id)};
  }
  const auto load = [&](std::size_t i) {
    return MySqlClanStore(configuration).Load(identities[i]);
  };
  const auto member = [&](std::size_t i, unsigned role) {
    sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES (" +
        std::to_string(identities[i].character_id) + ",7000," +
        std::to_string(role) + ")");
  };
  check(!MySqlClanStore(configuration).Load({}));
  check(!MySqlClanStore(configuration)
             .Load({identities[1].account_id, identities[0].character_id}));
  auto snapshot = load(0);
  check(snapshot && snapshot->profile.clan_id == 0 && !snapshot->details);
  check(snapshot->profile.disband_date_bits ==
        std::bit_cast<std::uint64_t>(73051.0));
  sql("INSERT INTO Clans(ClanId,NameUtf16,IntroductionUtf16,MemberCapacity,MarkId,ClanLevel,ClanPoints,IsRecruiting,CreatedAtUnixSeconds) VALUES (7000,UNHEX('540065007300740043006C0061006E00'),UNHEX('480065006C006C006F00'),30,42,7,1234,1,1704067200)");
  member(0, 101);
  member(1, 102);
  member(2, 100);
  member(3, 1);
  for (std::size_t i = 0; i < identities.size(); ++i) {
    snapshot = load(i);
    check(snapshot && snapshot->profile.clan_id == 7000 &&
          snapshot->profile.clan_name == u"TestClan");
    check(snapshot->profile.details_available == (i == 3 ? 0 : 1));
    check(snapshot->details.has_value() == (i != 3));
  }
  snapshot = load(0);
  const auto& d = *snapshot->details;
  check(d.text_at76 == u"ClanMaster" && d.text_at100 == u"ClanDeputy" &&
        d.text_at200 == u"Hello");
  check(d.value_at132 == 7 && d.value_at136 == 1234 && d.value_at164 == 3 &&
        d.value_at168 == 30);
  check(d.recruiting == 1 && d.mark_id == 42 &&
        d.date_at144_bits == std::bit_cast<std::uint64_t>(45292.0));
  check(d.value_at124 == 0 && d.value_at128 == 0 && d.value_at172 == 0 &&
        d.value_at152_bits == 0 && d.text_at176.empty());
  sql("UPDATE Clans SET DisbandAtUnixSeconds=1704153600 WHERE ClanId=7000");
  check(load(0)->profile.disband_date_bits ==
        std::bit_cast<std::uint64_t>(45293.0));
  const auto disable = "UPDATE Accounts SET IsDisabled=1 WHERE AccountId=" +
                       std::to_string(identities[0].account_id);
  sql(disable);
  check(!load(0));
  sql("UPDATE Accounts SET IsDisabled=0 WHERE AccountId=" +
      std::to_string(identities[0].account_id));
  rejects([&] {
    sql("UPDATE ClanMembers SET MembershipStatus=101 WHERE CharacterId=" +
        std::to_string(identities[2].character_id));
  });
  rejects([&] {
    sql("UPDATE ClanMembers SET MembershipStatus=0 WHERE CharacterId=" +
        std::to_string(identities[2].character_id));
  });
  rejects([&] { sql("UPDATE Clans SET MemberCapacity=0 WHERE ClanId=7000"); });
  rejects([&] {
    sql("UPDATE Clans SET NameUtf16=UNHEX('4100') WHERE ClanId=7000");
  });
  rejects([&] {
    sql("UPDATE Clans SET CreatedAtUnixSeconds=4102444800 WHERE ClanId=7000");
  });
  sql("UPDATE Clans SET MemberCapacity=2 WHERE ClanId=7000");
  rejects([&] { load(0); });
  sql("UPDATE Clans SET MemberCapacity=30, IntroductionUtf16=UNHEX('0000') WHERE ClanId=7000");
  rejects([&] { load(0); });
  sql("UPDATE Clans SET IntroductionUtf16=UNHEX('48006900') WHERE ClanId=7000");
  sql("DELETE FROM ClanMembers WHERE CharacterId=" +
      std::to_string(identities[0].character_id));
  rejects([&] { load(1); });  // A persisted roster without a master is invalid.
  check(!load(0)->details && load(0)->profile.clan_id == 0);
  sql("DELETE FROM Clans WHERE ClanId=7000");
  check(load(1)->profile.clan_id == 0 && !load(3)->details);
  std::cout << "Clan persistence: " << checks << " checks passed\n";
}
}  // namespace bubble_fighter::server::tests
