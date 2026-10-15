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
    throw std::runtime_error("Clan training line " + std::to_string(at.line()));
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
template <class T, class Packet>
void Codec(const Packet& packet) {
  const auto encoded = T::Decode(packet).Encode();
  Check(encoded.message_id == packet.message_id &&
        encoded.payload == packet.payload);
  auto bad = packet;
  ++bad.message_id;
  Rejects([&] { T::Decode(bad); });
  for (std::size_t n = 0; n < packet.payload.size(); ++n) {
    bad = packet;
    bad.payload.resize(n);
    Rejects([&] { T::Decode(bad); });
  }
  bad = packet;
  bad.payload.push_back(0);
  Rejects([&] { T::Decode(bad); });
}
}  // namespace
int RunClanTrainingTests(const std::filesystem::path& directory) {
  checks = 0;
  std::ifstream input(directory / "clan_training_packets.txt");
  Check(input.good());
  std::string label, hex;
  std::uint16_t mid = 0;
  int fixtures = 0;
  while (input >> label >> mid >> hex) {
    p::ClanPacket packet{mid, {}};
    if (hex != "-")
      for (std::size_t i = 0; i < hex.size(); i += 2)
        packet.payload.push_back(static_cast<std::uint8_t>(
            std::stoul(hex.substr(i, 2), nullptr, 16)));
    if (mid == 1106)
      Codec<p::TrainingClanJoinRequest>(packet);
    else if (mid == 1107)
      Codec<p::TrainingClanJoinResponse>(packet);
    else if (mid == 1108)
      Codec<p::TrainingClanExitRequest>(packet);
    else if (mid == 948)
      Codec<p::ClanAppearanceReport>(p::TcpPacket{mid, packet.payload});
    else if (mid == 1515)
      Codec<p::TrainingClanGraduationNotification>(
          p::TcpPacket{mid, packet.payload});
    else
      Check(false);
    ++fixtures;
  }
  Check(fixtures == 13);
  Rejects([] { p::TrainingClanJoinResponse{0, 100, u"훈련소"}.Encode(); });
  Rejects([] { p::TrainingClanJoinResponse{0xffffffff, 1, {}}.Encode(); });
  Rejects([] {
    p::TrainingClanJoinResponse{99, 100, std::u16string(u"A\0B", 3)}.Encode();
  });
  p::TrainingClanJoinResponse maximum{
      99, 100,
      std::u16string((p::ClanPacketCodec::kMaxPayloadSize - 10) / 2, u'A')};
  const auto large = maximum.Encode();
  Check(p::TrainingClanJoinResponse::Decode(large).clan_name ==
        maximum.clan_name);
  maximum.clan_name += u'A';
  Rejects([&] { maximum.Encode(); });
  auto auth = std::make_shared<c::ClanAuthentication>();
  const c::ClanIdentity identity{1, 10};
  auto game = auth->BindGame(identity);
  c::ClanSnapshot snapshot;
  snapshot.training_exit_notice =
      p::ClanProfileResponse{0, 99, 0xffffffecU, u"훈련소", 0};
  snapshot.training_exit_generation = 100;
  int calls = 0, fallback = 0;
  bool revoke = false;
  auto make = [&] {
    return Session(
        auth,
        [&](const auto&) -> std::optional<c::ClanSnapshot> { return snapshot; },
        c::ClanAuthentication::Clock::now, {}, {}, {},
        [&](const auto&, const auto&, const auto&) {
          ++fallback;
          return c::ClanMutationResult{};
        },
        {}, {},
        [&](const auto& id, c::TrainingClanAction action,
            const auto& authorized) {
          Check(id == identity && authorized());
          ++calls;
          if (revoke) {
            game.reset();
            Check(!authorized());
            throw std::runtime_error("Synthetic revocation");
          }
          c::TrainingClanResult result;
          result.handled = action != c::TrainingClanAction::kManualLeave;
          result.mutation.responses.push_back(
              p::TrainingClanJoinResponse{}.Encode());
          return result;
        });
  };
  auto login = [&](auto& session) {
    Check(session
              .Process(
                  p::ClanLoginRequest{auth->Issue(game, u"Fighter")}.Encode())
              .responses.size() == 1);
  };
  auto session = make();
  login(session);
  Check(session.Process(p::ClanProfileRequest{}.Encode()).responses.size() ==
        2);
  Check(session.Process(p::ClanProfileRequest{}.Encode()).responses.size() ==
        1);
  ++snapshot.training_exit_generation;
  Check(session.Process(p::ClanDetailsRequest{}.Encode()).responses.size() ==
        2);
  Check(
      session.Process(p::TrainingClanJoinRequest{}.Encode()).responses.size() ==
      1);
  for (auto status : {0xffffffecU, 0xffffffc4U, 7U})
    Check(session.Process(p::TrainingClanExitRequest{status}.Encode())
              .responses.size() == 1);
  Check(session.Process(p::ClanLeaveRequest{}.Encode()).responses.empty());
  Check(calls == 5 && fallback == 1);
  auto replacement = make();
  login(replacement);
  Check(session.Poll().close_connection);
  Check(
      replacement.Process(p::ClanProfileRequest{}.Encode()).responses.size() ==
      2);
  revoke = true;
  Rejects([&] { replacement.Process(p::TrainingClanJoinRequest{}.Encode()); });
  Check(replacement.Poll().close_connection);
  game = auth->BindGame(identity);
  auto malformed = make();
  login(malformed);
  Rejects([&] { malformed.Process({1108, {1}}); });
  auto unauthenticated = make();
  Rejects(
      [&] { unauthenticated.Process(p::TrainingClanJoinRequest{}.Encode()); });
  std::cout << "Clan training codec/session: " << checks << " checks passed\n";
  return checks;
}
