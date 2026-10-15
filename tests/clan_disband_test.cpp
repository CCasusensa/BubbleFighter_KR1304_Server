#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>

#include "sessions/clan_session.h"

namespace {
namespace p = bubble_fighter::server::protocol;
namespace c = bubble_fighter::server::features::clans;
using Session = bubble_fighter::server::sessions::ClanSession;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Clan disband line " + std::to_string(at.line()));
}
template <class F>
void Rejects(F f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception&) {
    failed = true;
  }
  Check(failed);
}
template <class T>
void Codec(const p::ClanPacket& packet) {
  Check(T::Decode(packet).Encode() == packet);
  auto bad = packet;
  ++bad.message_id;
  Rejects([&] { T::Decode(bad); });
  for (std::size_t size = 0; size < packet.payload.size(); ++size) {
    bad = packet;
    bad.payload.resize(size);
    Rejects([&] { T::Decode(bad); });
  }
  bad = packet;
  bad.payload.push_back(0);
  Rejects([&] { T::Decode(bad); });
}
}  // namespace
int RunClanDisbandTests(const std::filesystem::path& directory) {
  checks = 0;
  std::ifstream input(directory / "clan_disband_packets.txt");
  Check(input.good());
  std::string label, hex;
  std::uint16_t mid = 0;
  int fixtures = 0;
  while (input >> label >> mid >> hex) {
    p::ClanPacket packet{mid, {}};
    for (std::size_t i = 0; i < hex.size(); i += 2)
      packet.payload.push_back(
          static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    if (mid == 1029)
      Codec<p::ClanDisbandRequest>(packet);
    else if (mid == 1030)
      Codec<p::ClanDisbandResponse>(packet);
    else if (mid == 1031)
      Codec<p::ClanDisbandNotification>(packet);
    else
      Check(false);
    ++fixtures;
  }
  Check(fixtures == 13);
  Check(p::ClanDisbandRequest{2}.Encode().payload ==
        std::vector<std::uint8_t>({2, 0, 0, 0}));
  Check(p::ClanDisbandResponse{1, 2}.Encode().payload ==
        std::vector<std::uint8_t>({1, 0, 0, 0, 2, 0, 0, 0}));
  auto auth = std::make_shared<c::ClanAuthentication>();
  const c::ClanIdentity identity{1, 10};
  auto game = auth->BindGame(identity);
  c::ClanSnapshot snapshot;
  snapshot.disband_notice =
      p::ClanProfileResponse{0, 7500, 0xffffffd8U, u"Gone", 0};
  int calls = 0;
  bool revoke = false;
  auto make = [&] {
    return Session(
        auth,
        [&](const auto& id) -> std::optional<c::ClanSnapshot> {
          Check(id == identity);
          return snapshot;
        },
        c::ClanAuthentication::Clock::now, {}, {}, {}, {}, {},
        [&](const auto& id, const auto& request, const auto& authorized) {
          Check(id == identity && authorized());
          ++calls;
          if (revoke) {
            auth->UpdateScene(game, {60, 0});
            auth->UpdateScene(game, {60, 3});
            Check(!authorized());
            throw std::runtime_error("Synthetic scene change");
          }
          return c::ClanMutationResult{
              {p::ClanDisbandResponse{1, request.action}.Encode()}, {}};
        });
  };
  auto session = make();
  auto login = [&](Session& target) {
    Check(
        target
            .Process(p::ClanLoginRequest{auth->Issue(game, u"Master")}.Encode())
            .responses.size() == 1);
  };
  login(session);
  auto result = session.Process(p::ClanProfileRequest{}.Encode());
  Check(result.responses.size() == 2);
  Check(p::ClanProfileResponse::Decode(result.responses[0]).membership_status ==
        0xffffffd8U);
  Check(p::ClanProfileResponse::Decode(result.responses[1]).membership_status ==
        0);
  Check(session.Process(p::ClanDetailsRequest{}.Encode()).responses.size() ==
        1);
  result = session.Process(p::ClanDisbandRequest{1}.Encode());
  Check(p::ClanDisbandResponse::Decode(result.responses[0]).result == 0 &&
        calls == 0);
  auth->UpdateScene(game, {60, 3});
  result = session.Process(p::ClanDisbandRequest{1}.Encode());
  Check(p::ClanDisbandResponse::Decode(result.responses[0]).result == 1 &&
        calls == 1);
  auto replacement = make();
  login(replacement);
  Check(session.Poll().close_connection);
  Check(
      replacement.Process(p::ClanProfileRequest{}.Encode()).responses.size() ==
      2);
  revoke = true;
  Rejects([&] { replacement.Process(p::ClanDisbandRequest{2}.Encode()); });
  Check(replacement.Poll().close_connection);
  auto malformed = make();
  login(malformed);
  Rejects([&] { malformed.Process({1029, {1}}); });
  Check(malformed.Poll().close_connection);
  auto unauthorized = make();
  Rejects([&] { unauthorized.Process(p::ClanDisbandRequest{1}.Encode()); });
  std::cout << "Clan disband codec/session: " << checks << " checks passed\n";
  return checks;
}
