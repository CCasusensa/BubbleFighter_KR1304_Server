#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <source_location>
#include <sstream>
#include <stdexcept>

#include "features/authentication/credential_crypto.h"
#include "features/characters/character_profile.h"
#include "features/inventory/item_use.h"
#include "features/rooms/match_profile_updates.h"
#include "features/rooms/room_map_catalog.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_match_store.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/rooms/create_room_request.h"
#include "protocol/rooms/join_room_request.h"
#include "protocol/rooms/room_ready_request.h"
#include "protocol/unresolved/client_to_server_message_0x0209.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bf = bubble_fighter::server;
namespace proto = bf::protocol;
namespace rooms = bf::features::rooms;
namespace chars = bf::features::characters;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Settlement profile check at " +
                             std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected);
}
struct Store final : chars::CharacterStore {
  chars::CharacterRecord record;
  int reads = 0, fault = 0;
  std::shared_ptr<chars::CharacterStore> database;
  std::optional<chars::CharacterRecord> FindByAccount(
      std::uint64_t account) override {
    ++reads;
    if (fault == 1) throw std::runtime_error("Injected profile read failure");
    if (fault == 2) return {};
    if (database) return database->FindByAccount(account);
    auto value = record;
    if (fault == 3) ++value.account_id;
    if (fault == 4) ++value.character_id;
    if (fault == 5) value.statistics.level = 0;
    if (fault == 6) value.character_name = u"Changed";
    if (fault == 7) value.character_type = 27;
    if (fault == 8) value.color_index = 2;
    Check(account == record.account_id);
    return value;
  }
  bool NameExists(std::u16string_view) override { return false; }
  chars::CharacterCreateResult Create(chars::CharacterRecord&) override {
    throw std::logic_error("Unexpected create");
  }
};
struct Fixture {
  using Session = bf::sessions::PlayerSession;
  rooms::RoomGameSession::Time now{};
  std::array<std::shared_ptr<Store>, 2> stores;
  std::array<std::unique_ptr<Session>, 2> players;
  std::optional<rooms::RoomMatchRequest> request;
  std::optional<rooms::RoomMatchResult> receipt;
  int settlements = 0;
  std::shared_ptr<rooms::RoomRegistry> registry;
  std::uint16_t room_id = 0;
  std::filesystem::path configuration;
  explicit Fixture(chars::CharacterStatistics before,
                   const std::filesystem::path& config = {}, int trial = 0)
      : configuration(config) {
    for (std::size_t i = 0; i < 2; ++i) {
      stores[i] = std::make_shared<Store>();
      stores[i]->record = {100 + i, 200 + i, i == 0 ? u"Host" : u"Guest",
                           10,      7,       before};
    }
    if (!configuration.empty()) {
      namespace sql = bf::persistence::my_sql_commands;
      bf::persistence::MySqlAccountStore accounts(configuration);
      auto database =
          std::make_shared<bf::persistence::MySqlCharacterStore>(configuration);
      auto password =
          bf::features::authentication::CredentialCrypto::HashPassword(
              "Synthetic-profile-password");
      for (std::size_t i = 0; i < stores.size(); ++i) {
        auto& store = stores[i];
        const auto username =
            "profile_" + std::to_string(trial) + "_" + std::to_string(i);
        Check(accounts.CreateAccount(username, password));
        store->record.character_id = 0;
        store->record.account_id = accounts.FindAccount(username)->account_id;
        store->record.character_name += static_cast<char16_t>(u'0' + trial);
        Check(database->Create(store->record) ==
              chars::CharacterCreateResult::kCreated);
        bf::persistence::MySqlConnection connection(configuration);
        sql::Statement fund(
            connection.Handle(),
            "UPDATE Characters SET Level=?, Experience=?, Lucci=?, Cash=? WHERE CharacterId=?");
        std::array values{sql::Number(before.level),
                          sql::Number(before.experience),
                          sql::Number(before.lucci), sql::Number(before.cash),
                          sql::Number(store->record.character_id)};
        fund.Execute(values);
        store->record.statistics = before;
        store->database = database;
      }
    }
    registry = rooms::RoomRegistry::Create(
        4, [this] { return now; }, [] { return 123; },
        [this](const rooms::RoomMatchRequest& pending) {
          ++settlements;
          if (!configuration.empty()) {
            auto committed =
                bf::persistence::MySqlMatchStore(configuration).Settle(pending);
            if (!receipt) {
              request = pending;
              receipt = committed;
              throw std::runtime_error(
                  "MySQL committed receipt; first delivery lost");
            }
            Check(committed.Encode() == receipt->Encode());
            return committed;
          }
          if (!receipt) {
            request = pending;
            rooms::RoomMatchResult result;
            for (const auto& member : pending.members) {
              auto c = stores.at(member.slot)->record;
              const auto initial = c.statistics;
              c.statistics = pending.rules.Apply(
                  initial, member.scores, member.team == pending.Winner());
              // Persisted result remains historical even if a live character
              // was renamed since it was committed. It must not replace room
              // identity.
              c.character_name = member.slot == 0 ? u"OldHost" : u"OldGuest";
              result.rewards.push_back({member.slot, c, initial});
            }
            result.Validate(pending);
            receipt = rooms::RoomMatchResult::Decode(result.Encode());
            throw std::runtime_error("Committed receipt; first delivery lost");
          }
          Check(pending.Encode() == request->Encode());
          return *receipt;
        });
    for (std::size_t i = 0; i < 2; ++i) {
      const auto store = stores[i];
      players[i] = std::make_unique<Session>(
          [store](std::string_view) {
            return std::optional{store->record.account_id};
          },
          store,
          [](const auto& c, auto at) {
            return bf::sessions::CreateMenuSnapshot(
                c, {c.account_id, c.character_id, {}}, at);
          },
          Session::EquipmentChangeHandler{}, Session::ShopCatalogProvider{},
          Session::ShopPurchaseHandler{}, Session::CashPickupHandler{},
          Session::WeaponPresetHandler{}, Session::ItemUseHandler{},
          [this] { return now; }, [] { return 123; }, registry);
      proto::ClientToServerMessage0x0001 initial;
      initial.initial_value = 1256;
      players[i]->Process(initial.Encode());
      proto::LoginRequest login;
      login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
      players[i]->Process(login.Encode());
      players[i]->Process(proto::ClientToServerMessage0x0209{}.Encode());
      players[i]->Process({106, {}});
    }
    proto::CreateRoomRequest create;
    create.settings.text_at8 = u"Fresh";
    create.settings.configuration.short_at4 = 1;
    auto created = players[0]->Process(create.Encode());
    room_id =
        proto::GotoRoomNotification::Decode(created.at(1)).descriptor.short_at0;
    players[1]->Process(proto::JoinRoomRequest{room_id, {}}.Encode());
    players[0]->Poll();
    players[1]->Process(proto::RoomReadyRequest{1, 0, 0}.Encode());
    players[0]->Poll();
    const auto& map = rooms::RoomMapCatalog::Maps().front();
    proto::StartGameRequest start{0,
                                  std::u16string(map.directory),
                                  std::u16string(map.name),
                                  std::u16string(map.key),
                                  map.difficulty,
                                  map.rules,
                                  0};
    players[0]->Process(start.Encode());
    players[1]->Poll();
    for (auto& p : players)
      p->Process(proto::GameLoadingCompleteRequest{}.Encode());
    for (auto& p : players) p->Poll();
    now += std::chrono::seconds(4);
    for (auto& p : players) p->Poll();
    now += std::chrono::minutes(10);
    for (auto& p : players) p->Poll();
    now += std::chrono::seconds(10);
    for (auto& p : players) p->Poll();
    Check(settlements == 1 && receipt.has_value());
    for (const auto& p : players)
      Check(p->State() == bf::sessions::PlayerSessionState::kInRoom);
  }
  std::vector<proto::TcpPacket> Deliver(std::size_t who, bool process) {
    return process ? players[who]->Process({65535, {}}) : players[who]->Poll();
  }
};
void VerifyCurrent(Fixture& f, std::size_t i,
                   std::vector<proto::TcpPacket> packets,
                   const chars::CharacterStatistics& expected) {
  int profiles = 0, scores = 0;
  const auto old_bytes = f.receipt->Encode();
  rooms::BindMatchRewardProfiles(f.request->rules, packets);
  for (const auto& p : packets) {
    if (p.message_id == 614) {
      ++scores;
      Check(p.payload == f.receipt->Scores(*f.request).Encode().payload);
    }
    if (p.message_id != 105) continue;
    ++profiles;
    const auto profile = proto::UserStatNotification::Decode(p);
    Check(profile.values[1] == expected.experience);
    Check(profile.values[4] == expected.lucci);
    Check(profile.values[5] == expected.cash);
    Check(profile.fighter.values_at32[1] == expected.level);
    const auto range = f.request->rules.ExperienceRange(expected.experience);
    Check(profile.values[2] == range[0] && profile.values[3] == range[1]);
  }
  Check(profiles == 1 && scores == 1);
  Check(f.players[i]->Character()->statistics == expected);
  Check(f.registry->Find(f.room_id)
            ->FindSlot(static_cast<std::uint32_t>(i))
            ->character.statistics == expected);
  Check(f.players[i]->Character()->character_name ==
        f.stores[i]->record.character_name);
  Check(f.receipt->Encode() == old_bytes);
}
}  // namespace
int RunSettlementProfileTests(const std::filesystem::path& directory) {
  std::ifstream input(directory / "settlement_profile_cases.txt");
  Check(static_cast<bool>(input));
  std::string line;
  int cases = 0;
  while (std::getline(input, line)) {
    if (line.empty() || line.front() == '#') continue;
    std::istringstream row(line);
    std::string label;
    chars::CharacterStatistics before, current;
    row >> label >> before.level >> before.experience >> before.lucci >>
        before.cash >> current.level >> current.experience >> current.lucci >>
        current.cash;
    Check(!row.fail());
    for (bool process : {false, true}) {
      Fixture f(before);
      const auto saved = f.receipt->Encode();
      for (auto& s : f.stores) s->record.statistics = current;
      f.now += std::chrono::seconds(2);
      const int reads = f.stores[0]->reads;
      VerifyCurrent(f, 0, f.Deliver(0, process), current);
      Check(f.stores[0]->reads == reads + 1);
      // Guest drains an already queued receipt after another trusted update.
      auto later = current;
      later.cash = current.cash == 0 ? 1 : current.cash - 1;
      f.stores[1]->record.statistics = later;
      VerifyCurrent(f, 1, f.Deliver(1, !process), later);
      Check(f.settlements == 2 && f.receipt->Encode() == saved);
      const int stable_reads = f.stores[0]->reads;
      Check(f.Deliver(0, process).empty());
      Check(f.stores[0]->reads == stable_reads);
      Check(f.players[0]->Character()->statistics == current);
    }
    ++cases;
  }
  Check(cases == 6);
  for (bool process : {false, true})
    for (int fault = 1; fault <= 8; ++fault) {
      Fixture f({1, 0, 200, 30});
      f.stores[0]->fault = fault;
      f.now += std::chrono::seconds(2);
      const auto saved = f.receipt->Encode();
      Rejects([&] { f.Deliver(0, process); });
      Check(f.players[0]->State() ==
            bf::sessions::PlayerSessionState::kRejected);
      Check(!f.players[0]->Character() && !f.players[0]->AccountId());
      Check(f.receipt->Encode() == saved && f.settlements == 2);
      Check(f.players[1]->State() == bf::sessions::PlayerSessionState::kInRoom);
    }
  std::cout << "Settlement profile: " << checks << " checks passed\n";
  return checks;
}

int RunSettlementProfilePersistenceTests(
    const std::filesystem::path& configuration) {
  namespace db = bf::persistence;
  namespace sql = db::my_sql_commands;
  for (int trial = 0; trial < 2; ++trial) {
    Fixture f({1, 0, 200, 30}, configuration, trial);
    const auto saved = f.receipt->Encode();
    for (const auto& store : f.stores) {
      auto fresh = store->database->FindByAccount(store->record.account_id);
      Check(fresh->statistics.experience == 100 &&
            fresh->statistics.lucci == 250);
    }
    // A trusted writer commits after rewards but before the response retry.
    // Both character reads and the immutable replay use production MySQL
    // stores.
    const auto change = [&](std::size_t i, std::uint32_t cash) {
      db::MySqlConnection connection(configuration);
      sql::Statement update(
          connection.Handle(),
          "UPDATE Characters SET Lucci=7, Cash=? WHERE CharacterId=?");
      std::array values{sql::Number(cash),
                        sql::Number(f.stores[i]->record.character_id)};
      update.Execute(values);
      Check(update.AffectedRows() == 1);
    };
    change(0, 3);
    change(1, 3);
    f.now += std::chrono::seconds(2);
    VerifyCurrent(f, 0, f.Deliver(0, trial == 1), {1, 100, 7, 3});
    change(1, 2);
    VerifyCurrent(f, 1, f.Deliver(1, trial == 0), {1, 100, 7, 2});
    // Reopening persistence reproduces old receipt bytes, without restoring
    // balances or losing the current session/room profile.
    Check(db::MySqlMatchStore(configuration).Settle(*f.request).Encode() ==
          saved);
    Check(db::MySqlCharacterStore(configuration)
              .FindByAccount(f.stores[0]->record.account_id)
              ->statistics == chars::CharacterStatistics{1, 100, 7, 3});
    Check(db::MySqlCharacterStore(configuration)
              .FindByAccount(f.stores[1]->record.account_id)
              ->statistics == chars::CharacterStatistics{1, 100, 7, 2});
  }
  std::cout << "Settlement profile MySQL: " << checks << " checks passed\n";
  return checks;
}

int RunBattleRecordDeliveryTests() {
  const auto before_checks = checks;
  for (const bool process : {false, true}) {
    Fixture f({1, 0, 200, 30});
    const auto receipt = f.receipt->Encode();
    for (std::size_t i = 0; i < 43; ++i) {
      f.stores[0]->record.battle.values[i] = static_cast<unsigned>(11 + i);
      f.stores[1]->record.battle.values[i] = static_cast<unsigned>(51 + i);
    }
    f.stores[0]->record.battle.Reset(1);
    f.now += std::chrono::seconds(2);
    const auto verify = [&](std::size_t i, bool drain) {
      auto packets = f.Deliver(i, drain);
      int profiles = 0;
      for (const auto& packet : packets) {
        if (packet.message_id != 105) continue;
        ++profiles;
        const auto current =
            proto::UserStatNotification::Decode(packet).fighter;
        const auto& expected = f.stores[i]->record.battle.values;
        Check(current.values_at32[2] == expected[0]);
        Check(current.values_at80[0] == expected[1]);
        Check(current.values_at80[29] == expected[12]);
        Check(current.values_at80[47] == expected[21]);
        Check(current.values_at80[52] == expected[26]);
        Check(current.values_at404[6] == expected[33]);
        Check(current.values_at464[3] == expected[38]);
      }
      Check(profiles == 1);
      Check(f.players[i]->Character()->battle == f.stores[i]->record.battle);
      Check(f.registry->Find(f.room_id)
                ->FindSlot(static_cast<unsigned>(i))
                ->character.battle == f.stores[i]->record.battle);
      Check(f.receipt->Encode() == receipt);
    };
    verify(0, process);
    // The second queue was encoded before this reset and drains afterward.
    f.stores[1]->record.battle.Reset(2);
    verify(1, !process);
    Check(f.settlements == 2);
  }
  const auto added = checks - before_checks;
  std::cout << "Battle record delivery: " << added << " checks passed\n";
  return added;
}
