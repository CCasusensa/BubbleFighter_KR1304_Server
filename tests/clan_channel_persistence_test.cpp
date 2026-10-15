#include <array>
#include <iostream>
#include <source_location>

#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::tests {
void RunClanChannelPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace features;
  int checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Clan channel persistence line " +
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
      "Synthetic-channel-password");
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto username = "clan_channel_" + std::to_string(i);
    check(accounts.CreateAccount(username, password));
    characters::CharacterRecord member{
        0, accounts.FindAccount(username)->account_id,
        u"Channel" + std::u16string(1, static_cast<char16_t>(u'A' + i)), 27, 2};
    check(characters.Create(member) ==
          characters::CharacterCreateResult::kCreated);
    ids[i] = {member.account_id,
              static_cast<std::uint32_t>(member.character_id)};
  }
  const auto cid = [&](int i) { return std::to_string(ids[i].character_id); };
  auto rows = store.ReadChannelMembers(ids);
  check(rows && rows->size() == 4);
  for (std::size_t i = 0; i < 4; ++i)
    check(rows->at(i).identity == ids[i] && rows->at(i).enabled &&
          rows->at(i).level == 1 && rows->at(i).clan_id == 0 &&
          rows->at(i).membership_status == 0);
  check(!store.ReadChannelMembers({}));
  check(!store.ReadChannelMembers(std::array{ids[0], ids[0]}));
  check(!store.ReadChannelMembers(
      std::array{clans::ClanIdentity{ids[0].account_id, ids[1].character_id}}));
  check(!store.ReadChannelMembers(
      std::array{clans::ClanIdentity{ids[0].account_id, 0xffffffffU}}));
  sql("INSERT INTO Clans(ClanId,NameUtf16,IntroductionUtf16,MemberCapacity,CreatedAtUnixSeconds) VALUES(7701,X'4300680061006E006E0065006C00',X'48006900',30,1704067200)");
  for (int i = 0; i < 3; ++i)
    sql("INSERT INTO ClanMembers(ClanId,CharacterId,MembershipStatus) VALUES(7701," +
        cid(i) + "," +
        (i == 0   ? "101"
         : i == 1 ? "100"
                  : "1") +
        ")");
  sql("UPDATE Characters SET Level=50 WHERE CharacterId=" + cid(1));
  sql("UPDATE Accounts SET IsDisabled=1 WHERE AccountId=" +
      std::to_string(ids[3].account_id));
  auto auth = std::make_shared<clans::ClanAuthentication>();
  clans::ClanChannelAccess gate(
      auth, [&](auto group) { return store.ReadChannelMembers(group); });
  rows = store.ReadChannelMembers(ids);
  check(rows && rows->at(1).level == 50 && !rows->at(3).enabled);
  check(rows->at(0).clan_id == 7701 && rows->at(0).membership_status == 101 &&
        rows->at(2).membership_status == 1);
  const auto group = std::span(ids).first(3);
  check(gate.Authorize(80, 1, 260, 1, group).result == 0);
  check(gate.Authorize(80, 1, 260, 0, group).result == 4);
  check(gate.Authorize(80, 1, 49, 1, group).result == 2);
  check(gate.Authorize(80, 1, 260, 3, ids).result == 2);
  check(gate.Authorize(80, 1, 260, 1, std::span(ids).subspan(2, 1)).result ==
        3);
  const auto reverse = std::array{ids[1], ids[0]};
  rows = store.ReadChannelMembers(reverse);
  check(rows && rows->at(0).identity == ids[1] &&
        rows->at(1).identity == ids[0]);
  // The source closes its read transaction before the scene callback. A fresh
  // independent connection can update the same row while the callback runs.
  bool wrote = false;
  check(
      gate.Authorize(
              80, 1, 260, 1, group,
              [&] {
                sql("UPDATE ClanMembers SET MembershipStatus=100 WHERE CharacterId=" +
                    cid(2));
                wrote = true;
                return true;
              })
              .result == 0 &&
      wrote);
  check(gate.Authorize(80, 1, 260, 0, group).result == 0);
  sql("DELETE FROM ClanMembers WHERE CharacterId=" + cid(0));
  check(gate.Authorize(80, 1, 260, 1, group).result == 3);
  // Failed/missing snapshots also roll back and leave the reusable connection
  // clean.
  check(!store.ReadChannelMembers(
      std::array{ids[0], clans::ClanIdentity{99999999, 99999999}}));
  check(store.ReadChannelMembers(std::span(ids).first(1))->front().clan_id ==
        0);
  sql("DELETE FROM Clans WHERE ClanId=7701");
  std::cout << "Clan channel persistence: " << checks << " checks passed\n";
}
}  // namespace bubble_fighter::server::tests
