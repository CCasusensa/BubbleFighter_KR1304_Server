#include <array>
#include <iostream>
#include <source_location>

#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::tests {
void RunClanPlazaPersistenceTests(const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace features;
  int checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Clan plaza persistence line " +
                               std::to_string(at.line()));
  };
  MySqlConnection connection(configuration);
  const auto sql = [&](const std::string& text) {
    my_sql_commands::Statement q(connection.Handle(), text);
    q.Execute({});
  };
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  MySqlClanStore store(configuration);
  std::array<clans::ClanIdentity, 4> ids;
  const auto password = authentication::CredentialCrypto::HashPassword(
      "Synthetic-plaza-password");
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto username = "clan_plaza_" + std::to_string(i);
    check(accounts.CreateAccount(username, password));
    characters::CharacterRecord member{
        0, accounts.FindAccount(username)->account_id,
        u"Plaza" + std::u16string(1, static_cast<char16_t>(u'A' + i)), 27, 2};
    check(characters.Create(member) ==
          characters::CharacterCreateResult::kCreated);
    ids[i] = {member.account_id,
              static_cast<std::uint32_t>(member.character_id)};
  }
  const auto cid = [&](int i) { return std::to_string(ids[i].character_id); };
  check(!store.ReadPlazaMembers(ids));
  check(!store.ReadPlazaMembers({}));
  check(!store.ReadPlazaMembers(std::array{ids[0], ids[0]}));
  check(!store.ReadPlazaMembers(
      std::array{clans::ClanIdentity{ids[0].account_id, ids[1].character_id}}));
  sql("INSERT INTO Clans(ClanId,NameUtf16,IntroductionUtf16,MemberCapacity,CreatedAtUnixSeconds) VALUES(7801,X'50006C0061007A006100',X'48006900',30,1704067200),(7802,X'4F007400680065007200',X'48006900',30,1704067200)");
  for (int i = 0; i < 3; ++i)
    sql("INSERT INTO ClanMembers(ClanId,CharacterId,MembershipStatus) VALUES(7801," +
        cid(i) + "," +
        (i == 0   ? "101"
         : i == 1 ? "100"
                  : "1") +
        ")");
  const auto group = std::span(ids).first(3);
  auto rows = store.ReadPlazaMembers(group);
  check(rows && rows->size() == 3);
  for (std::size_t i = 0; i < 3; ++i) {
    check(rows->at(i).member.identity == ids[i] && rows->at(i).member.enabled);
    check(rows->at(i).clan_name == u"Plaza" &&
          rows->at(i).member.clan_id == 7801);
  }
  auto auth = std::make_shared<clans::ClanAuthentication>();
  clans::ClanPlazaAccess gate(
      auth, [&](auto members) { return store.ReadPlazaMembers(members); });
  check(!gate.Authorize(7801, 60, 1, 260, group).allowed);
  sql("UPDATE ClanMembers SET MembershipStatus=102 WHERE CharacterId=" +
      cid(2));
  bool committed = false;
  check(
      gate.Authorize(7801, 60, 1, 260, group,
                     [&](const auto& scope) {
                       check(scope.clan_id == 7801 && scope.name == u"Plaza");
                       // The read transaction has ended. An independent
                       // connection sees and can change the persistent row
                       // while the scene callback runs.
                       sql("UPDATE Characters SET Level=45 WHERE CharacterId=" +
                           cid(1));
                       committed = true;
                       return true;
                     })
          .allowed &&
      committed);
  check(!gate.Authorize(7801, 60, 1, 44, group).allowed);
  check(gate.Authorize(7801, 60, 1, 45, group).allowed);
  rows = store.ReadPlazaMembers(std::array{ids[2], ids[1], ids[0]});
  check(rows && rows->at(0).member.identity == ids[2] &&
        rows->at(2).member.identity == ids[0]);
  sql("UPDATE Accounts SET IsDisabled=1 WHERE AccountId=" +
      std::to_string(ids[1].account_id));
  check(!gate.Authorize(7801, 60, 1, 260, group).allowed);
  sql("UPDATE Accounts SET IsDisabled=0 WHERE AccountId=" +
      std::to_string(ids[1].account_id));
  sql("UPDATE ClanMembers SET ClanId=7802 WHERE CharacterId=" + cid(1));
  check(!gate.Authorize(7801, 60, 1, 260, group).allowed);
  check(gate.Authorize(7802, 60, 1, 260, std::span(ids).subspan(1, 1)).allowed);
  sql("UPDATE ClanMembers SET ClanId=7801 WHERE CharacterId=" + cid(1));
  sql("UPDATE Clans SET NameUtf16=X'520065006E0061006D0065006400' WHERE ClanId=7801");
  check(gate.Authorize(7801, 60, 1, 260, group,
                       [&](const auto& scope) {
                         check(scope.name == u"Renamed");
                         return true;
                       })
            .allowed);
  // The schema accepts UTF16 bytes, while authority additionally rejects an
  // invalid canonical display name. No reported client name can repair it.
  sql("UPDATE Clans SET NameUtf16=X'42006100640020004E0061006D006500' WHERE ClanId=7801");
  check(!gate.Authorize(7801, 60, 1, 260, group).allowed);
  sql("UPDATE Clans SET NameUtf16=X'50006C0061007A006100' WHERE ClanId=7801");
  check(!store.ReadPlazaMembers(
      std::array{ids[0], clans::ClanIdentity{99999999, 99999999}}));
  check(gate.Authorize(7801, 60, 1, 260, group).allowed);
  check(!gate.Authorize(7801, 60, 1, 260, group,
                        [](const auto&) { return false; })
             .allowed);
  sql("DELETE FROM Clans WHERE ClanId=7801");
  check(!store.ReadPlazaMembers(group));
  check(!gate.Authorize(7801, 60, 1, 260, group).allowed);
  sql("DELETE FROM Clans WHERE ClanId=7802");
  std::cout << "Clan plaza persistence: " << checks << " checks passed\n";
}
}  // namespace bubble_fighter::server::tests
