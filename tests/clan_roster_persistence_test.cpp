#include <array>
#include <iostream>
#include <source_location>

#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::tests {
void RunClanRosterPersistenceTests(const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace features;
  using namespace protocol;
  using Kind = clans::ClanRosterKind;
  int checks = 0;
  const auto check = [&](bool value, std::source_location at =
                                         std::source_location::current()) {
    ++checks;
    if (!value)
      throw std::runtime_error("Clan roster persistence line " +
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
      "Synthetic-roster-password");
  std::array<clans::ClanIdentity, 6> identities;
  for (std::size_t i = 0; i < identities.size(); ++i) {
    const auto username = "clan_roster_" + std::to_string(i);
    check(accounts.CreateAccount(username, password));
    const auto name =
        u"Roster" + std::u16string(1, static_cast<char16_t>(u'A' + i));
    characters::CharacterRecord row{
        0, accounts.FindAccount(username)->account_id, name, 27, 2};
    check(characters.Create(row) ==
          characters::CharacterCreateResult::kCreated);
    identities[i] = {row.account_id,
                     static_cast<std::uint32_t>(row.character_id)};
  }
  MySqlClanStore store(configuration);
  const auto read = [&](std::size_t actor, Kind kind) {
    return store.ReadRoster(identities[actor], kind).value();
  };
  const auto resync = [&](std::size_t actor, Kind kind, std::uint32_t role) {
    const auto replies = read(actor, kind);
    check(replies.size() == 1);
    const auto profile = ClanProfileResponse::Decode(replies[0]);
    check(profile.membership_status == role &&
          profile.clan_id == (role ? 7200u : 0u));
  };
  for (auto kind : {Kind::kMembers, Kind::kDetails, Kind::kApplicants})
    resync(5, kind, 0);
  sql("INSERT INTO Clans(ClanId,NameUtf16,IntroductionUtf16,MemberCapacity,IsRecruiting,CreatedAtUnixSeconds) VALUES(7200,UNHEX('52006F007300740065007200'),UNHEX('48006900'),30,1,1704067200)");
  for (std::size_t i = 0; i < 5; ++i) {
    const auto role = std::array{101, 102, 100, 1, 1}[i];
    sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES(" +
        std::to_string(identities[i].character_id) + ",7200," +
        std::to_string(role) + ")");
  }
  for (std::size_t actor = 0; actor < 3; ++actor) {
    const auto result = read(actor, Kind::kMembers);
    check(result.size() == 1);
    const auto members = ClanRosterResponse::Decode(result[0]).members;
    check(members.size() == 3);
    const auto extended = read(actor, Kind::kDetails);
    check(extended.size() == 1);
    const auto details = ClanMemberDetailsResponse::Decode(extended[0]);
    check(details.value_at0 == 0 && details.members.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
      const auto role =
          static_cast<std::uint32_t>(std::array{101, 102, 100}[i]);
      const auto name =
          u"Roster" + std::u16string(1, static_cast<char16_t>(u'A' + i));
      check(members[i] ==
            ClanMemberRecord{identities[i].character_id, name, 0, 0, role});
      ClanMemberDetailsRecord expected;
      expected.character_id = identities[i].character_id;
      expected.character_name = name;
      expected.membership_status = role;
      check(details.members[i] == expected);
    }
  }
  for (auto kind : {Kind::kMembers, Kind::kDetails, Kind::kApplicants}) {
    resync(3, kind, 1);
    resync(5, kind, 0);
    check(!store.ReadRoster({}, kind));
    check(!store.ReadRoster(
        {identities[0].account_id, identities[1].character_id}, kind));
  }
  resync(2, Kind::kApplicants, 100);
  for (auto mode : {0, 1}) {
    sql("UPDATE Clans SET IsRecruiting=" + std::to_string(mode) +
        " WHERE ClanId=7200");
    for (auto actor : {0u, 1u}) {
      const auto replies = read(actor, Kind::kApplicants);
      check(replies.size() == 2);
      check(ClanDetailsResponse::Decode(replies[0]).recruiting ==
            static_cast<std::uint32_t>(mode));
      const auto pending =
          ClanApplicantsResponse::Decode(replies[1]).applicants;
      check(pending.size() == 2);
      for (std::size_t i = 0; i < 2; ++i) {
        ClanApplicantRecord expected;
        expected.character_id = identities[i + 3].character_id;
        expected.character_name =
            u"Roster" + std::u16string(1, static_cast<char16_t>(u'D' + i));
        expected.membership_status = 1;
        check(pending[i] == expected);
      }
    }
  }
  sql("UPDATE Accounts SET IsDisabled=1 WHERE Username='clan_roster_0'");
  for (auto kind : {Kind::kMembers, Kind::kDetails, Kind::kApplicants})
    check(!store.ReadRoster(identities[0], kind));
  sql("UPDATE Accounts SET IsDisabled=0 WHERE Username='clan_roster_0'");
  sql("UPDATE ClanMembers SET MembershipStatus=100 WHERE CharacterId=" +
      std::to_string(identities[1].character_id));
  resync(1, Kind::kApplicants, 100);
  sql("UPDATE Characters SET NameUtf16=UNHEX('0000') WHERE CharacterId=" +
      std::to_string(identities[4].character_id));
  rejects([&] { read(0, Kind::kApplicants); });
  sql("UPDATE Characters SET NameUtf16=UNHEX('52006F0073007400650072004500') WHERE CharacterId=" +
      std::to_string(identities[4].character_id));
  check(ClanApplicantsResponse::Decode(read(0, Kind::kApplicants)[1])
            .applicants.size() == 2);
  sql("UPDATE Clans SET MemberCapacity=1 WHERE ClanId=7200");
  for (auto kind : {Kind::kMembers, Kind::kDetails, Kind::kApplicants})
    rejects([&] { read(0, kind); });
  sql("UPDATE Clans SET MemberCapacity=65535 WHERE ClanId=7200");
  rejects([&] { read(0, static_cast<Kind>(99)); });
  // Independently fixed frame arithmetic: 16-unit names give 50/94/70-byte
  // rows. Keep only the master; no mutation API is involved in this isolated
  // fixture.
  sql("DELETE FROM ClanMembers WHERE ClanId=7200 AND MembershipStatus<>101");
  std::string account_values, character_values, member_values;
  for (int i = 0; i < 1310; ++i) {
    const auto id = std::to_string(2000000 + i);
    auto name = std::string("RosterBulk") +
                std::string(6 - std::to_string(i).size(), '0') +
                std::to_string(i);
    std::string hex;
    constexpr char digits[] = "0123456789ABCDEF";
    for (unsigned char c : name) {
      hex += digits[c >> 4];
      hex += digits[c & 15];
      hex += "00";
    }
    if (i) {
      account_values += ',';
      character_values += ',';
      member_values += ',';
    }
    account_values += "(" + id + ",'roster_bulk_" + std::to_string(i) +
                      "',UNHEX(REPEAT('00',16)),UNHEX(REPEAT('00',32)),600000)";
    character_values += "(" + id + "," + id + ",UNHEX('" + hex + "'),27,2)";
    member_values += "(" + id + ",7200,100)";
  }
  sql("INSERT INTO Accounts(AccountId,Username,PasswordSalt,PasswordHash,PasswordIterations) VALUES" +
      account_values);
  sql("INSERT INTO Characters(CharacterId,AccountId,NameUtf16,CharacterType,ColorIndex) VALUES" +
      character_values);
  sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES" +
      member_values);
  // One shorter master (32 bytes), 1,310 * 50 + 4 = 65,536: reject as a whole.
  rejects([&] { read(0, Kind::kMembers); });
  sql("DELETE FROM ClanMembers WHERE CharacterId=2001309");
  check(ClanRosterResponse::Decode(read(0, Kind::kMembers)[0]).members.size() ==
        1310);
  // Master is 76 bytes; 696 * 94 + 8 + 76 = 65,508; one more exceeds capacity.
  sql("DELETE FROM ClanMembers WHERE CharacterId>=2000696");
  check(ClanMemberDetailsResponse::Decode(read(0, Kind::kDetails)[0])
            .members.size() == 697);
  sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES(2000696,7200,100)");
  rejects([&] { read(0, Kind::kDetails); });
  sql("DELETE FROM ClanMembers WHERE CharacterId>=2000000");
  sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) SELECT CharacterId,7200,1 FROM Characters WHERE CharacterId BETWEEN 2000000 AND 2000935");
  check(ClanApplicantsResponse::Decode(read(0, Kind::kApplicants)[1])
            .applicants.size() == 936);
  sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES(2000936,7200,1)");
  rejects([&] { read(0, Kind::kApplicants); });
  sql("DELETE FROM ClanMembers WHERE CharacterId>=2000000");
  check(ClanApplicantsResponse::Decode(read(0, Kind::kApplicants)[1])
            .applicants.empty());
  sql("DELETE FROM Clans WHERE ClanId=7200");
  sql("DELETE FROM Characters WHERE CharacterId BETWEEN 2000000 AND 2001309");
  sql("DELETE FROM Accounts WHERE AccountId BETWEEN 2000000 AND 2001309");
  resync(0, Kind::kMembers, 0);
  std::cout << "Clan roster persistence: " << checks << " checks passed\n";
}
}  // namespace bubble_fighter::server::tests
