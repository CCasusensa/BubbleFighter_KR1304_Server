#include "features/clans/clan_authentication.h"

#include <array>
#include <atomic>
#include <barrier>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "protocol/clans/clan_state_report.h"
#include "protocol/inventory/item_use_response.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "sessions/clan_session.h"
#include "sessions/player_session.h"

namespace clans = bubble_fighter::server::features::clans;
namespace proto = bubble_fighter::server::protocol;
namespace sessions = bubble_fighter::server::sessions;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Clan authentication line " +
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
using Auth = clans::ClanAuthentication;
struct Fixture {
  Auth::Clock::time_point time{};
  // Deliberately repeat random output: unique serials still prevent replay.
  std::shared_ptr<Auth> auth =
      std::make_shared<Auth>([this] { return time; },
                             [] {
                               std::array<std::uint8_t, 32> secret{};
                               secret.fill(0x5a);
                               return secret;
                             });
  Auth::GameLink game = auth->BindGame({44, 71});
  proto::ClanCredentials Issue() { return auth->Issue(game, u"Fighter"); }
};

void Credentials() {
  Fixture f;
  const auto first = f.Issue();
  Check(first.value_at0 == 71 && first.text_at4 == u"Fighter" &&
        first.value_at28 == 1 && first.value_at32 == 0);
  Check(first.passport.size() == 40 && first.passport[0] == 1);
  for (std::size_t i = 1; i < 8; ++i) Check(first.passport[i] == 0);
  for (std::size_t i = 8; i < 40; ++i) Check(first.passport[i] == 0x5a);
  auto accepted = f.auth->Consume(first);
  Check(accepted.Identity() == std::optional<clans::ClanIdentity>{{44, 71}});
  Check(!f.auth->Consume(first).Identity());
  const auto next = f.Issue();
  Check(next.passport != first.passport);
  Check(accepted.Identity().has_value());
  auto bad = next;
  ++bad.value_at0;
  Check(!f.auth->Consume(bad).Identity());
  Check(!f.auth->Consume(next).Identity());
  Check(accepted.Identity().has_value());
  const auto newer = f.Issue();
  auto replacement = f.auth->Consume(newer);
  Check(!accepted.Identity() && replacement.Identity().has_value());
  for (int field = 0; field < 4; ++field) {
    auto credential = f.Issue();
    const auto original = credential;
    switch (field) {
      case 0:
        ++credential.value_at0;
        break;
      case 1:
        credential.text_at4.push_back(u'X');
        break;
      case 2:
        ++credential.value_at28;
        break;
      case 3:
        ++credential.value_at32;
        break;
    }
    Check(!f.auth->Consume(credential).Identity());
    Check(!f.auth->Consume(original).Identity());
    Check(replacement.Identity().has_value());
  }
  const auto superseded = f.Issue();
  const auto current = f.Issue();
  Check(!f.auth->Consume(superseded).Identity());
  bad = current;
  bad.passport[12] ^= 1;
  Check(!f.auth->Consume(bad).Identity());
  bad = current;
  bad.passport.pop_back();
  Check(!f.auth->Consume(bad).Identity());
  bad = current;
  bad.passport.push_back(0);
  Check(!f.auth->Consume(bad).Identity());
  Check(f.auth->Consume(current).Identity().has_value());
  const auto timed = f.Issue();
  f.time += std::chrono::seconds{30};
  Check(!f.auth->Consume(timed).Identity());
  const auto live = f.Issue();
  f.time += std::chrono::seconds{29};
  auto last = f.auth->Consume(live);
  Check(last.Identity().has_value());
  const auto disconnected = f.Issue();
  f.game.reset();
  Check(!last.Identity());
  Check(!f.auth->Consume(disconnected).Identity());
  // A new main generation for the same account cannot revive old proofs.
  f.game = f.auth->BindGame({44, 71});
  Check(!last.Identity() && !f.auth->Consume(live).Identity());
  Rejects([&] { f.auth->BindGame({44, 99}); });
  Rejects([&] { f.auth->BindGame({45, 71}); });
  Rejects([&] { f.auth->BindGame({0, 71}); });
  Rejects([&] { f.auth->BindGame({44, 0}); });
  Rejects([&] { f.auth->Issue({}, u"Fighter"); });
  Rejects([&] { f.auth->Issue(f.game, u""); });
  Auth other;
  Rejects([&] { other.Issue(f.game, u"Fighter"); });
}

void Competition() {
  Fixture f;
  for (int round = 0; round < 16; ++round) {
    const auto credentials = f.Issue();
    std::array<Auth::Binding, 8> results;
    std::barrier start(8);
    std::vector<std::jthread> threads;
    for (std::size_t i = 0; i < results.size(); ++i) {
      threads.emplace_back([&, i] {
        start.arrive_and_wait();
        results[i] = f.auth->Consume(credentials);
      });
    }
    threads.clear();
    int accepted = 0;
    for (auto& result : results)
      if (result.Identity()) ++accepted;
    Check(accepted == 1);
  }
  std::vector<Auth::GameLink> links;
  for (std::uint32_t i = 1; i <= 127; ++i)
    links.push_back(f.auth->BindGame({1000 + i, 2000 + i}));
  Rejects([&] { f.auth->BindGame({5000, 5000}); });
  links.pop_back();
  Check(static_cast<bool>(f.auth->BindGame({5000, 5000})));
}

void Sessions() {
  Fixture f;
  bool available = true;
  int reads = 0;
  clans::ClanSnapshot snapshot;
  auto provider = [&](const clans::ClanIdentity& identity)
      -> std::optional<clans::ClanSnapshot> {
    Check(identity == clans::ClanIdentity{44, 71});
    ++reads;
    return available ? std::optional{snapshot} : std::nullopt;
  };
  auto now = [&] { return f.time; };
  sessions::ClanSession session(f.auth, provider, now);
  Check(session.Process(proto::ClanHeartbeatRequest{}.Encode())
            .responses.empty());
  const auto login =
      session.Process(proto::ClanLoginRequest{f.Issue()}.Encode());
  Check(!login.close_connection && login.responses.size() == 1 && reads == 1);
  Check(proto::ClanLoginResponse::Decode(login.responses[0]).result == 1);
  const auto profile = session.Process(proto::ClanProfileRequest{}.Encode());
  Check(profile.responses ==
        std::vector<proto::ClanPacket>{snapshot.profile.Encode()});
  Check(reads == 2);
  snapshot.profile = {1, 1000, 101, u"ClanName", 0};
  snapshot.details.emplace();
  snapshot.details->clan_name = u"ClanName";
  const auto details = session.Process(proto::ClanDetailsRequest{}.Encode());
  Check(details.responses ==
        std::vector<proto::ClanPacket>{snapshot.details->Encode()});
  const auto refused = session.Process(
      proto::ClanCreateRequest{u"NewClan", 0, u"Intro", 100819, 30}.Encode());
  Check(proto::ClanCreateResponse::Decode(refused.responses.at(0)).result == 0);
  snapshot = {};
  Check(session.Process(proto::ClanDetailsRequest{}.Encode()).responses ==
        std::vector<proto::ClanPacket>{snapshot.profile.Encode()});
  available = false;
  Check(session.Process(proto::ClanProfileRequest{}.Encode()).close_connection);
  Check(session.Poll().close_connection);
  available = true;
  sessions::ClanSession unauthenticated(f.auth, provider, now);
  Rejects(
      [&] { unauthenticated.Process(proto::ClanProfileRequest{}.Encode()); });
  Check(unauthenticated.Poll().close_connection);
  sessions::ClanSession timed(f.auth, provider, now);
  f.time += std::chrono::seconds{9};
  Check(
      !timed.Process(proto::ClanHeartbeatRequest{}.Encode()).close_connection);
  f.time += std::chrono::seconds{1};
  Check(timed.Poll().close_connection);
  sessions::ClanSession invalid(f.auth, provider, now);
  auto denied = invalid.Process(proto::ClanLoginRequest{}.Encode());
  Check(denied.close_connection);
  Check(proto::ClanLoginResponse::Decode(denied.responses.at(0)).result ==
        0xffffffffU);
  sessions::ClanSession bound(f.auth, provider, now);
  Check(!bound.Process(proto::ClanLoginRequest{f.Issue()}.Encode())
             .close_connection);
  f.game.reset();
  Check(bound.Poll().close_connection);
}

void ReadBoundaries() {
  for (int action = 0; action < 3; ++action) {
    Fixture f;
    sessions::ClanSession session(
        f.auth,
        [&](auto) -> std::optional<clans::ClanSnapshot> {
          if (action == 0) f.game.reset();
          if (action == 1) f.time += std::chrono::seconds{11};
          if (action == 2)
            throw std::runtime_error("Synthetic database failure");
          return clans::ClanSnapshot{};
        },
        [&] { return f.time; });
    auto packet = proto::ClanLoginRequest{f.Issue()}.Encode();
    if (action == 2)
      Rejects([&] { session.Process(packet); });
    else {
      auto result = session.Process(packet);
      Check(result.close_connection &&
            proto::ClanLoginResponse::Decode(result.responses.at(0)).result ==
                0xffffffffU);
    }
    Check(session.Poll().close_connection);
  }
}

struct Characters final
    : bubble_fighter::server::features::characters::CharacterStore {
  using Record = bubble_fighter::server::features::characters::CharacterRecord;
  Record character{71, 44, u"Fighter", 27, 2};
  std::optional<Record> FindByAccount(std::uint64_t id) override {
    return id == character.account_id ? std::optional{character} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bubble_fighter::server::features::characters::CharacterCreateResult Create(
      Record&) override {
    throw std::logic_error("Unexpected character creation");
  }
};

void MainSessions() {
  using Practice = bubble_fighter::server::features::practice::PracticeSession;
  auto authentication = std::make_shared<Auth>();
  auto characters = std::make_shared<Characters>();
  int confirmations = 0;
  const auto create = [&] {
    return std::unique_ptr<sessions::PlayerSession>(new sessions::PlayerSession(
        [](auto) { return std::optional<std::uint64_t>{44}; }, characters, {},
        {}, {}, {}, {}, {}, {}, Practice::Clock::now,
        Practice::CreateRandomSeed, {}, {}, {}, {}, {}, {}, {}, authentication,
        [&](const clans::ClanIdentity& identity,
            const proto::ItemUseRequest& request, std::uint64_t observed) {
          ++confirmations;
          Check(identity == clans::ClanIdentity{44, 71} &&
                observed > 1704067200);
          if (request.value_at4 != 1 || !request.arguments.empty())
            throw std::runtime_error("Invalid synthetic confirmation");
          return std::vector<proto::TcpPacket>{
              proto::ItemUseResponse{0, request.item_id, {}}.Encode()};
        }));
  };
  const auto login = [&](auto& player) {
    proto::ClientToServerMessage0x0001 initial;
    initial.initial_value = 1256;
    Check(player.Process(initial.Encode()).size() == 2);
    proto::LoginRequest request;
    request.nexon_passport = u"BFS1." + std::u16string(64, u'0');
    Check(player.Process(request.Encode()).size() == 2);
  };
  const auto issue = [&](auto& player) {
    auto packets = player.Process(proto::ClanPassportRequest{}.Encode());
    Check(packets.size() == 1 && packets[0].message_id == 21);
    return proto::ClanPassportResponse::Decode(packets[0]).credentials;
  };
  auto player = create();
  Rejects([&] { issue(*player); });
  Check(player->State() == sessions::PlayerSessionState::kRejected);
  player = create();
  login(*player);
  auto credentials = issue(*player);
  const auto before_report = player->State();
  Check(
      player
          ->Process(proto::ClanStateReport{999999, u"SpoofClan", 101}.Encode())
          .empty());
  Check(player->State() == before_report);
  Check(player->Process(proto::ClanStateReport{0, u"", 0xfffffff6}.Encode())
            .empty());
  const auto confirmation = proto::ItemUseRequest{100819, 1, {}}.Encode();
  auto confirmed = player->Process(confirmation);
  Check(confirmed.size() == 1 &&
        proto::ItemUseResponse::Decode(confirmed[0]).item_id == 100819 &&
        confirmations == 1);
  auto binding = authentication->Consume(credentials);
  Check(binding.Identity() == std::optional<clans::ClanIdentity>{{44, 71}});
  auto outstanding = issue(*player);
  player.reset();
  Check(!binding.Identity() &&
        !authentication->Consume(outstanding).Identity());
  player = create();
  login(*player);
  binding = authentication->Consume(issue(*player));
  Rejects([&] { player->Process({25, {1}}); });
  Check(!binding.Identity());
  player = create();
  login(*player);
  auto second = create();
  login(*second);
  binding = authentication->Consume(issue(*player));
  Rejects([&] { issue(*second); });
  Check(binding.Identity().has_value());
  second.reset();
  player.reset();
  player = create();
  login(*player);
  Rejects([&] { player->Process(confirmation); });
  Check(confirmations == 1);
  player = create();
  login(*player);
  issue(*player);
  Rejects(
      [&] { player->Process(proto::ItemUseRequest{100819, 0, {}}.Encode()); });
  Check(confirmations == 2 &&
        player->State() == sessions::PlayerSessionState::kRejected);
  player = create();
  Rejects([&] { player->Process(proto::ClanStateReport{}.Encode()); });
  player = create();
  login(*player);
  Rejects([&] { player->Process(proto::ClanStateReport{}.Encode()); });
  player = create();
  login(*player);
  issue(*player);
  Rejects([&] { player->Process({2025, {0}}); });
}
void TrainingMain() {
  using Practice = bubble_fighter::server::features::practice::PracticeSession;
  auto auth = std::make_shared<Auth>();
  auto characters = std::make_shared<Characters>();
  auto time = Practice::Clock::now();
  bool graduated = false;
  int polls = 0;
  const auto make = [&] {
    return std::make_unique<sessions::PlayerSession>(
        [](auto) { return std::optional<std::uint64_t>{44}; }, characters,
        sessions::PlayerSession::MenuDataProvider{},
        sessions::PlayerSession::EquipmentChangeHandler{},
        sessions::PlayerSession::ShopCatalogProvider{},
        sessions::PlayerSession::ShopPurchaseHandler{},
        sessions::PlayerSession::CashPickupHandler{},
        sessions::PlayerSession::WeaponPresetHandler{},
        sessions::PlayerSession::ItemUseHandler{}, [&] { return time; },
        Practice::CreateRandomSeed, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, auth, clans::ClanConfirmationHandler{},
        [&](const clans::ClanIdentity& identity) {
          Check(identity == clans::ClanIdentity{44, 71});
          ++polls;
          return graduated;
        });
  };
  const auto login = [&](auto& player) {
    proto::ClientToServerMessage0x0001 initial;
    initial.initial_value = 1256;
    player.Process(initial.Encode());
    proto::LoginRequest request;
    request.nexon_passport = u"BFS1." + std::u16string(64, u'0');
    player.Process(request.Encode());
    player.Process(proto::ClanPassportRequest{}.Encode());
  };
  auto player = make();
  Rejects([&] { player->Process(proto::ClanAppearanceReport{}.Encode()); });
  player = make();
  login(*player);
  const auto state = player->State();
  Check(player
            ->Process(
                proto::ClanAppearanceReport{999999, 0xffffffff, 49}.Encode())
            .empty());
  Check(player->State() == state && polls == 0);
  Check(player->Poll().empty() && polls == 1);
  graduated = true;
  time += std::chrono::seconds{9};
  Check(player->Poll().empty() && polls == 1);
  time += std::chrono::seconds{1};
  const auto notification = player->Poll();
  Check(notification.size() == 1 && notification[0].message_id == 1515 &&
        notification[0].payload.empty() && polls == 2);
  time += std::chrono::seconds{30};
  Check(player->Poll().empty() && polls == 2);
  player.reset();
  player = make();
  login(*player);
  Check(player->Poll().at(0).message_id == 1515 && polls == 3);
  Rejects([&] { player->Process({948, {0}}); });
}
}  // namespace

int RunClanAuthenticationTests() {
  Credentials();
  Competition();
  Sessions();
  ReadBoundaries();
  MainSessions();
  TrainingMain();
  std::cout << "Clan authentication: " << checks << " checks passed\n";
  return checks;
}
