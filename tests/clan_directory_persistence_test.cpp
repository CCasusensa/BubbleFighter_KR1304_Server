#include <array>
#include <bit>
#include <iostream>
#include <source_location>

#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::tests {
void RunClanDirectoryPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace features;
  int checks = 0;
  const auto check = [&](bool value, std::source_location at =
                                         std::source_location::current()) {
    ++checks;
    if (!value)
      throw std::runtime_error("Clan directory persistence line " +
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
      "Synthetic-directory-password");
  std::array<clans::ClanIdentity, 8> identities;
  for (std::size_t i = 0; i < identities.size(); ++i) {
    const auto username = "clan_directory_" + std::to_string(i);
    check(accounts.CreateAccount(username, password));
    const auto name =
        u"Directory" + std::u16string(1, static_cast<char16_t>(u'A' + i));
    characters::CharacterRecord c{0, accounts.FindAccount(username)->account_id,
                                  name, 27, 2};
    check(characters.Create(c) == characters::CharacterCreateResult::kCreated);
    identities[i] = {c.account_id, static_cast<std::uint32_t>(c.character_id)};
  }
  const auto search = [&](std::uint32_t mode, std::uint32_t page = 1,
                          std::u16string name = u"") {
    return MySqlClanStore(configuration)
        .Search(identities[7], {mode, page, std::move(name)});
  };
  check(search(6)->clans.empty());
  for (std::uint32_t i = 0; i < 7; ++i) {
    // Fixed UTF-16LE fixture names Dir0..Dir6 and Hello.
    sql("INSERT INTO Clans(ClanId,NameUtf16,IntroductionUtf16,MemberCapacity,MarkId,ClanLevel,ClanPoints,IsRecruiting,CreatedAtUnixSeconds) VALUES(" +
        std::to_string(7100 + i) + ",UNHEX('4400690072003" + std::to_string(i) +
        "00'),UNHEX('480065006C006C006F00'),30,42,7," +
        std::to_string(i * 100) + "," + std::to_string(i % 2) + "," +
        std::to_string(1704067200ULL + i * 86400ULL) + ")");
    sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES(" +
        std::to_string(identities[i].character_id) + "," +
        std::to_string(7100 + i) + ",101)");
  }
  const auto ids = [](const auto& response) {
    std::vector<std::uint32_t> result;
    for (const auto& row : response->clans) result.push_back(row.clan_id);
    return result;
  };
  for (const auto mode : {5u, 6u}) {
    check(ids(search(mode)) ==
          std::vector<std::uint32_t>{7106, 7105, 7104, 7103, 7102});
    check(ids(search(mode, 2)) == std::vector<std::uint32_t>{7101, 7100});
    check(search(mode, 3)->clans.empty());
    check(search(mode, 10000)->clans.empty());
  }
  for (const auto mode : {7u, 8u})
    check(ids(search(mode)) == std::vector<std::uint32_t>{7105, 7103, 7101});
  auto found = search(4, 1, u"Dir0");
  check(found && found->value_at4 == 0 && found->page == 1 &&
        found->text_at12.empty() && found->clans.size() == 1);
  const auto& row = found->clans.at(0);
  check(row.clan_id == 7100 && row.clan_name == u"Dir0" &&
        row.master_name == u"DirectoryA");
  check(row.level == 7 && row.points == 0 && row.member_count == 1 &&
        row.mark_id == 42 && row.recruitment_mode == 0);
  check(row.introduction == u"Hello" &&
        row.created_at_bits == std::bit_cast<std::uint64_t>(45292.0));
  check(row.disband_at_bits == std::bit_cast<std::uint64_t>(73051.0) &&
        row.value_at88 == 0 && row.value_at56_bits == 0 &&
        row.value_after_member_count == 0 && row.last_login_bits == 0);
  check(search(4, 2, u"Dir0")->clans.empty());
  for (auto text : {u"dir0", u"Dir", u"%", u"'", u"Missing"})
    check(search(4, 1, text)->clans.empty());
  sql("UPDATE Clans SET ClanPoints=600 WHERE ClanId=7103");
  check(ids(search(5)) ==
        std::vector<std::uint32_t>{7103, 7106, 7105, 7104, 7102});
  sql("UPDATE Clans SET ClanPoints=300, CreatedAtUnixSeconds=1704585600 WHERE ClanId=7103");
  check(ids(search(6)) ==
        std::vector<std::uint32_t>{7106, 7103, 7105, 7104, 7102});
  sql("UPDATE Clans SET CreatedAtUnixSeconds=1704326400 WHERE ClanId=7103");
  sql("UPDATE Clans SET DisbandAtUnixSeconds=1704153600 WHERE ClanId=7100");
  check(search(4, 1, u"Dir0")->clans[0].disband_at_bits ==
        std::bit_cast<std::uint64_t>(45293.0));
  sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES(" +
      std::to_string(identities[7].character_id) + ",7100,1)");
  check(search(4, 1, u"Dir0")->clans[0].member_count == 1);
  sql("UPDATE ClanMembers SET MembershipStatus=100 WHERE CharacterId=" +
      std::to_string(identities[7].character_id));
  check(search(4, 1, u"Dir0")->clans[0].member_count == 2);
  sql("UPDATE Clans SET MemberCapacity=1 WHERE ClanId=7100");
  rejects([&] { search(4, 1, u"Dir0"); });
  sql("UPDATE Clans SET MemberCapacity=30 WHERE ClanId=7100");
  check(!MySqlClanStore(configuration).Search({}, {6, 1, u""}));
  check(!MySqlClanStore(configuration)
             .Search({identities[0].account_id, identities[7].character_id},
                     {6, 1, u""}));
  sql("UPDATE Accounts SET IsDisabled=1 WHERE AccountId=" +
      std::to_string(identities[7].account_id));
  check(!search(6));
  sql("UPDATE Accounts SET IsDisabled=0 WHERE AccountId=" +
      std::to_string(identities[7].account_id));
  check(search(6).has_value());
  for (const auto& query : std::vector<protocol::ClanDirectoryRequest>{
           {4, 0, u"Dir0"},
           {6, 10001, u""},
           {4, 1, u""},
           {4, 1, u"1234567890123"},
           {9, 1, u""},
           {6, 1, u"Dir0"},
           {4, 1, std::u16string{u'A', u'\0', u'B'}}})
    rejects(
        [&] { MySqlClanStore(configuration).Search(identities[7], query); });
  sql("UPDATE Clans SET IntroductionUtf16=UNHEX('0000') WHERE ClanId=7100");
  rejects([&] { search(4, 1, u"Dir0"); });
  sql("UPDATE Clans SET IntroductionUtf16=UNHEX('48006900') WHERE ClanId=7100");
  sql("DELETE FROM ClanMembers WHERE CharacterId=" +
      std::to_string(identities[0].character_id));
  rejects([&] { search(4, 1, u"Dir0"); });
  // Restore membership by a separate connection; a previous failed read has
  // released its locks and cannot poison the next read transaction.
  sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES(" +
      std::to_string(identities[0].character_id) + ",7100,101)");
  check(search(4, 1, u"Dir0")->clans[0].master_name == u"DirectoryA");
  sql("DELETE FROM Clans WHERE ClanId BETWEEN 7100 AND 7106");
  check(search(6)->clans.empty());
  std::cout << "Clan directory persistence: " << checks << " checks passed\n";
}
}  // namespace bubble_fighter::server::tests
