#include <array>
#include <barrier>
#include <iostream>
#include <source_location>
#include <thread>

#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_inventory_store.h"
#include "protocol/inventory/item_use_response.h"

namespace bubble_fighter::server::tests {
void SeedClanNetworkCards(const std::filesystem::path& configuration) {
  persistence::MySqlAccountStore accounts(configuration);
  persistence::MySqlCharacterStore characters(configuration);
  persistence::MySqlInventoryStore inventory(configuration);
  for (const auto user : {"net_host", "net_guest", "net_observer"}) {
    const auto account = accounts.FindAccount(user);
    if (!account) throw std::runtime_error("Missing isolated network account");
    const auto character = characters.FindByAccount(account->account_id);
    if (!character)
      throw std::runtime_error("Missing isolated network character");
    features::inventory::InventorySlot slot{
        100819, features::inventory::InventoryUsage::kCount, 2};
    if (!inventory.Save(account->account_id, character->character_id, slot,
                        1704067200))
      throw std::runtime_error("Could not seed isolated clan cards");
  }
  std::cout << "3 isolated clan card inventories created.\n";
}
void RunClanCreationPersistenceTests(
    const std::filesystem::path& configuration) {
  using namespace persistence;
  using namespace features;
  constexpr std::uint64_t observed = 1704067200;
  int checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Clan creation persistence line " +
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
    my_sql_commands::Statement query(connection.Handle(), statement);
    query.Execute({});
  };
  const auto scalar = [&](const std::string& statement) {
    my_sql_commands::Statement query(connection.Handle(), statement);
    query.Execute({});
    std::uint64_t value = 0;
    std::array rows{my_sql_commands::Number(value)};
    if (!query.Fetch(rows)) throw std::runtime_error("Missing test aggregate");
    return value;
  };
  MySqlAccountStore accounts(configuration);
  MySqlCharacterStore characters(configuration);
  MySqlInventoryStore inventory(configuration);
  const auto password =
      authentication::CredentialCrypto::HashPassword("Synthetic-clan-password");
  std::array<clans::ClanIdentity, 10> identities;
  for (std::size_t i = 0; i < identities.size(); ++i) {
    const auto username = "clan_create_" + std::to_string(i);
    check(accounts.CreateAccount(username, password));
    characters::CharacterRecord c{
        0, accounts.FindAccount(username)->account_id,
        u"Create" + std::u16string(1, static_cast<char16_t>(u'A' + i)), 27, 2};
    check(characters.Create(c) == characters::CharacterCreateResult::kCreated);
    identities[i] = {c.account_id, static_cast<std::uint32_t>(c.character_id)};
    inventory::InventorySlot slot{100819, inventory::InventoryUsage::kCount,
                                  i == 6 ? 1U : 2U};
    slot.text_at36 = u"Preserved";
    check(inventory.Save(c.account_id, c.character_id, slot, observed));
    check(slot.revision == 1);
  }
  const auto slot = [&](std::size_t i) {
    return inventory
        .Load(identities[i].account_id, identities[i].character_id, observed)
        ->slots.at(0);
  };
  const auto request = [](std::u16string name) {
    return protocol::ClanCreateRequest{std::move(name), 0, u"Hello world",
                                       100819, 30};
  };
  const auto create = [&](std::size_t i, std::u16string name) {
    return MySqlClanStore(configuration)
        .Create(identities[i], request(std::move(name)), observed,
                [] { return true; });
  };
  const protocol::ItemUseRequest confirmation{100819, 1, {}};
  rejects([&] {
    MySqlClanStore(configuration)
        .ConfirmCreation(identities[0], confirmation, observed);
  });
  sql("INSERT INTO TrainingClanHistory VALUES(" +
      std::to_string(identities[0].character_id) + ",1704067200,4294967236,1)");
  auto response = create(0, u"CreatedClan");
  check(scalar("SELECT ExitStatus FROM TrainingClanHistory WHERE CharacterId=" +
               std::to_string(identities[0].character_id)) == 0);
  check(scalar("SELECT Graduated FROM TrainingClanHistory WHERE CharacterId=" +
               std::to_string(identities[0].character_id)) == 1);
  check(response == protocol::ClanCreateResponse{1, 100819, 0});
  auto snapshot = MySqlClanStore(configuration).Load(identities[0]);
  check(snapshot && snapshot->profile.membership_status == 101 &&
        snapshot->profile.clan_id >= 1000);
  check(snapshot->details && snapshot->details->clan_name == u"CreatedClan" &&
        snapshot->details->value_at168 == 30 &&
        snapshot->details->value_at164 == 1 &&
        snapshot->details->text_at76 == u"CreateA" &&
        snapshot->details->text_at200 == u"Hello world");
  check(slot(0).quantity == 1 && slot(0).revision == 2 &&
        slot(0).text_at36 == u"Preserved");
  check(
      scalar(
          "SELECT COUNT(*) FROM ClanCreationReceipts WHERE InventoryRevision=2 AND ItemId=100819") ==
      1);
  for (int repeat = 0; repeat < 3; ++repeat) {
    const auto packets =
        MySqlClanStore(configuration)
            .ConfirmCreation(identities[0], confirmation, observed);
    check(packets.size() == 2 && packets[1].message_id == 802 &&
          packets[1].payload ==
              inventory
                  .Load(identities[0].account_id, identities[0].character_id,
                        observed)
                  ->CreateNotification(identities[0].account_id, observed)
                  .Encode()
                  .payload);
    check(protocol::ItemUseResponse::Decode(packets[0]) ==
          protocol::ItemUseResponse{0, 100819, {}});
    check(slot(0).quantity == 1 && slot(0).revision == 2);
  }
  check(create(0, u"OtherClan").result == 0xffffff97U);
  check(create(1, u"CreatedClan").result == 2 && slot(1).quantity == 2 &&
        slot(1).revision == 1);
  sql("INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES(" +
      std::to_string(identities[2].character_id) + "," +
      std::to_string(snapshot->profile.clan_id) + ",1)");
  check(create(2, u"PendingClan").result == 0xffffff98U &&
        slot(2).quantity == 2);
  for (const auto& invalid :
       {protocol::ClanCreateRequest{u"Bad", 0, u"Hi", 100819, 30},
        protocol::ClanCreateRequest{u"Valid", 0, u"", 100819, 30},
        protocol::ClanCreateRequest{u"Valid", 0, u"Hi", 100819, 31},
        protocol::ClanCreateRequest{u"Valid", 1, u"Hi", 100819, 30}}) {
    check(MySqlClanStore(configuration)
              .Create(identities[1], invalid, observed, [] { return true; })
              .result != 1);
    check(slot(1).quantity == 2 && slot(1).revision == 1);
  }
  rejects([&] {
    MySqlClanStore(configuration)
        .Create({identities[1].account_id, identities[0].character_id},
                request(u"WrongOwner"), observed, [] { return true; });
  });
  rejects([&] {
    MySqlClanStore(configuration)
        .Create(identities[1], request(u"WrongTime"), 4102444800ULL,
                [] { return true; });
  });
  for (auto invalid : {protocol::ItemUseRequest{100819, 0, {}},
                       protocol::ItemUseRequest{100819, 1, {u"Forged"}},
                       protocol::ItemUseRequest{100820, 1, {}}})
    rejects([&] {
      MySqlClanStore(configuration)
          .ConfirmCreation(identities[0], invalid, observed);
    });
  rejects([&] {
    MySqlClanStore(configuration)
        .ConfirmCreation({identities[1].account_id, identities[0].character_id},
                         confirmation, observed);
  });
  sql("UPDATE Accounts SET IsDisabled=1 WHERE AccountId=" +
      std::to_string(identities[1].account_id));
  rejects([&] { create(1, u"Disabled"); });
  sql("UPDATE Accounts SET IsDisabled=0 WHERE AccountId=" +
      std::to_string(identities[1].account_id));
  auto empty = slot(3);
  empty.quantity = 0;
  check(inventory.Save(identities[3].account_id, identities[3].character_id,
                       empty, observed));
  check(create(3, u"NoCards").result == 0 && slot(3).quantity == 0);
  sql("DELETE FROM InventorySlots WHERE CharacterId=" +
      std::to_string(identities[4].character_id));
  check(create(4, u"NoSlot").result == 0);
  const auto clan_count = scalar("SELECT COUNT(*) FROM Clans");
  const auto members = scalar("SELECT COUNT(*) FROM ClanMembers");
  const auto receipts = scalar("SELECT COUNT(*) FROM ClanCreationReceipts");
  // The harness installs a last-write failure trigger for clan_create_5.
  rejects([&] { create(5, u"Rollback"); });
  check(slot(5).quantity == 2 && slot(5).revision == 1);
  int authority_reads = 0;
  rejects([&] {
    MySqlClanStore(configuration)
        .Create(identities[1], request(u"RevokedClan"), observed,
                [&] { return ++authority_reads == 1; });
  });
  check(authority_reads == 2 && slot(1).quantity == 2 && slot(1).revision == 1);
  check(scalar("SELECT COUNT(*) FROM Clans") == clan_count &&
        scalar("SELECT COUNT(*) FROM ClanMembers") == members &&
        scalar("SELECT COUNT(*) FROM ClanCreationReceipts") == receipts);
  const auto race = [&](std::size_t first, std::size_t second, bool same_name) {
    std::barrier start(2);
    std::array<protocol::ClanCreateResponse, 2> results;
    std::array<std::exception_ptr, 2> errors;
    std::array<std::jthread, 2> workers;
    for (std::size_t i = 0; i < 2; ++i)
      workers[i] = std::jthread([&, i] {
        try {
          MySqlClanStore store(configuration);
          start.arrive_and_wait();
          results[i] = store.Create(identities[i == 0 ? first : second],
                                    request(same_name ? u"NameRace"
                                            : i == 0  ? u"RaceOne"
                                                      : u"RaceTwo"),
                                    observed, [] { return true; });
        } catch (...) {
          errors[i] = std::current_exception();
        }
      });
    for (auto& worker : workers) worker.join();
    for (auto error : errors)
      if (error) std::rethrow_exception(error);
    check((results[0].result == 1) != (results[1].result == 1));
    check((results[0].result == 1 ? results[1].result : results[0].result) ==
          (same_name ? 2U : 0xffffff97U));
  };
  race(6, 6, false);
  check(slot(6).quantity == 0 && slot(6).revision == 2);
  race(7, 8, true);
  check(slot(7).quantity + slot(8).quantity == 3 &&
        slot(7).revision + slot(8).revision == 3);
  // Historical receipts outlive membership; delayed confirmation only reads.
  sql("DELETE FROM Clans WHERE ClanId=" +
      std::to_string(snapshot->profile.clan_id));
  check(MySqlClanStore(configuration)
            .ConfirmCreation(identities[0], confirmation, observed)
            .size() == 2);
  check(slot(0).quantity == 1 && slot(0).revision == 2 &&
        !MySqlClanStore(configuration).Load(identities[0])->details);
  check(create(0, u"NextClan").result == 1 && slot(0).quantity == 0 &&
        slot(0).revision == 3);
  check(MySqlClanStore(configuration)
                .ConfirmCreation(identities[0], confirmation, observed)
                .size() == 2 &&
        slot(0).quantity == 0);
  check(scalar("SELECT COUNT(*) FROM ClanCreationReceipts WHERE CharacterId=" +
               std::to_string(identities[0].character_id)) == 2);
  sql("DELETE FROM Clans");
  // Credentials/rows live only in the random database deleted by the harness.
  std::cout << "Clan creation persistence: " << checks << " checks passed\n";
}
}  // namespace bubble_fighter::server::tests
