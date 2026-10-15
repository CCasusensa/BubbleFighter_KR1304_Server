#include <array>
#include <iostream>
#include <source_location>

#include "sessions/clan_session.h"

namespace {
namespace clans = bubble_fighter::server::features::clans;
namespace proto = bubble_fighter::server::protocol;
using Session = bubble_fighter::server::sessions::ClanSession;
using Auth = clans::ClanAuthentication;
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Clan roster query line " +
                             std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool failed = false;
  try {
    action();
  } catch (const std::exception&) {
    failed = true;
  }
  Check(failed);
}
auto Profiles(const clans::ClanIdentity&) {
  return std::optional{clans::ClanSnapshot{}};
}
const std::array<proto::ClanPacket, 3> requests{
    proto::ClanRosterRequest{}.Encode(),
    proto::ClanMemberDetailsRequest{}.Encode(),
    proto::ClanApplicantsRequest{}.Encode()};
}  // namespace

int RunClanRosterQueryTests() {
  for (std::size_t index = 0; index < requests.size(); ++index) {
    const auto& request = requests[index];
    auto auth = std::make_shared<Auth>();
    auto game = auth->BindGame({44, 71});
    int calls = 0;
    const std::vector<proto::ClanPacket> expected =
        index == 0   ? std::vector{proto::ClanRosterResponse{
                           {{71, u"Fighter", 0, 0,
                             101}}}.Encode()}
        : index == 1 ? std::vector{proto::ClanMemberDetailsResponse{}.Encode()}
                     : std::vector{proto::ClanDetailsResponse{}.Encode(),
                                   proto::ClanApplicantsResponse{}.Encode()};
    Session session(
        auth, Profiles, Auth::Clock::now, {}, {},
        [&](const clans::ClanIdentity& identity, clans::ClanRosterKind kind) {
          ++calls;
          Check(identity.account_id == 44 && identity.character_id == 71);
          Check(kind == static_cast<clans::ClanRosterKind>(index));
          return std::optional{expected};
        });
    Check(!session
               .Process(proto::ClanLoginRequest{auth->Issue(game, u"Fighter")}
                            .Encode())
               .close_connection);
    for (int repeat = 0; repeat < 2; ++repeat) {
      const auto result = session.Process(request);
      Check(!result.close_connection && result.responses == expected);
    }
    Check(calls == 2 && !session.Poll().close_connection);
    Check(session.Process(proto::ClanProfileRequest{}.Encode())
              .responses.size() == 1);
    for (int failure = 0; failure < 5; ++failure) {
      auto other_auth = std::make_shared<Auth>();
      auto other_game = other_auth->BindGame({44, 71});
      Session other(
          other_auth, Profiles, Auth::Clock::now, {}, {},
          [&](const auto&,
              auto) -> std::optional<std::vector<proto::ClanPacket>> {
            if (failure == 0) other_game = {};
            if (failure == 1) return {};
            if (failure == 2)
              throw std::runtime_error("Synthetic database error");
            auto replies = expected;
            if (failure == 3)
              replies.push_back({1070, std::vector<std::uint8_t>(65532)});
            return replies;
          });
      other.Process(
          proto::ClanLoginRequest{other_auth->Issue(other_game, u"Fighter")}
              .Encode());
      if (failure == 2 || failure == 3) {
        Rejects([&] { other.Process(request); });
      } else if (failure == 4) {
        other_game = {};
        const auto result = other.Process(request);
        Check(result.close_connection && result.responses.empty());
      } else {
        const auto result = other.Process(request);
        Check(result.close_connection && result.responses.empty());
      }
      Check(other.Poll().close_connection);
    }
    auto early_auth = std::make_shared<Auth>();
    Session early(early_auth, Profiles);
    Rejects([&] { early.Process(request); });
    Check(early.Poll().close_connection);
    auto missing_auth = std::make_shared<Auth>();
    auto missing_game = missing_auth->BindGame({44, 71});
    Session missing(missing_auth, Profiles);
    missing.Process(
        proto::ClanLoginRequest{missing_auth->Issue(missing_game, u"Fighter")}
            .Encode());
    Rejects([&] { missing.Process(request); });
    Check(missing.Poll().close_connection);
  }
  std::vector<proto::ClanPacket> malformed{{1011, {0}},
                                           {1069, {0}},
                                           {1013, {}},
                                           {1013, {1, 0, 0}},
                                           {1013, {1, 0, 0, 0, 0}},
                                           {1013, {0, 0, 0, 0}},
                                           {1013, {2, 0, 0, 0}},
                                           {1013, {255, 255, 255, 255}}};
  for (const auto& request : malformed) {
    auto auth = std::make_shared<Auth>();
    auto game = auth->BindGame({44, 71});
    int calls = 0;
    Session session(auth, Profiles, Auth::Clock::now, {}, {},
                    [&](const auto&, auto) {
                      ++calls;
                      return std::optional{std::vector<proto::ClanPacket>{}};
                    });
    session.Process(
        proto::ClanLoginRequest{auth->Issue(game, u"Fighter")}.Encode());
    Rejects([&] { session.Process(request); });
    Check(calls == 0 && session.Poll().close_connection);
  }
  std::cout << "Clan roster queries: " << checks << " checks passed\n";
  return checks;
}
