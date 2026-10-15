#include <array>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <thread>

#include "protocol/clans/clan_state_report.h"
#include "protocol/codec/packet_writer.h"
#include "sessions/clan_session.h"

namespace {
namespace clans = bubble_fighter::server::features::clans;
namespace proto = bubble_fighter::server::protocol;
using Auth = clans::ClanAuthentication;
using Session = bubble_fighter::server::sessions::ClanSession;
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Clan mutation line " + std::to_string(at.line()));
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
std::vector<std::uint8_t> Hex(const std::string& s) {
  std::vector<std::uint8_t> result;
  for (std::size_t i = 0; i < s.size(); i += 2)
    result.push_back(
        static_cast<std::uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
  return result;
}
void Reports(const std::filesystem::path& directory) {
  std::ifstream input(directory / "clan_state_report_packets.txt");
  Check(input.good());
  const std::array<proto::ClanStateReport, 4> expected{
      {{0, u"", 0},
       {1000, u"ClanA", 101},
       {0x12345678, u"战队한국", 1},
       {0xffffffff, u"A\U0001f4a7B", 0xfffffff6}}};
  std::string label, hex;
  std::uint16_t mid = 0;
  std::size_t index = 0;
  while (input >> label >> mid >> hex) {
    const proto::TcpPacket packet{mid, Hex(hex)};
    Check(index < expected.size());
    Check(proto::ClanStateReport::Decode(packet) == expected[index]);
    Check(expected[index++].Encode().payload == packet.payload);
    for (std::size_t length = 0; length < packet.payload.size(); ++length) {
      auto truncated = packet;
      truncated.payload.resize(length);
      Rejects([&] { proto::ClanStateReport::Decode(truncated); });
    }
    auto trailing = packet;
    trailing.payload.push_back(0);
    Rejects([&] { proto::ClanStateReport::Decode(trailing); });
    auto wrong = packet;
    wrong.message_id = 2026;
    Rejects([&] { proto::ClanStateReport::Decode(wrong); });
  }
  Check(index == 4 && input.eof());
  proto::ClanStateReport limit{
      0, std::u16string(proto::PacketWriter::kMaxStringCodeUnits, u'X'), 0};
  Check(proto::ClanStateReport::Decode(limit.Encode()) == limit);
  limit.clan_name.push_back(u'X');
  Rejects([&] { limit.Encode(); });
  Rejects([&] {
    proto::ClanStateReport::Decode({2025, {0, 0, 0, 0, 255, 255, 255, 255}});
  });
}
void Queues() {
  auto auth = std::make_shared<Auth>();
  auto actor = auth->BindGame({1, 10});
  auto recipient = auth->BindGame({2, 20});
  auto unrelated = auth->BindGame({3, 30});
  auto first = auth->Consume(auth->Issue(recipient, u"Recipient"));
  auto other = auth->Consume(auth->Issue(unrelated, u"Unrelated"));
  const auto packet = proto::ClanApplicationChangedNotification{}.Encode();
  const std::array<clans::ClanDelivery, 1> delivery{
      {{{20},
        std::make_shared<const std::vector<proto::ClanPacket>>(
            std::vector{packet})}}};
  auth->Publish(delivery);
  Check(first.TakeNotifications() == std::optional{std::vector{packet}});
  Check(first.TakeNotifications()->empty() &&
        other.TakeNotifications()->empty());
  for (int i = 0; i < 64; ++i) auth->Publish(delivery);
  Check(first.TakeNotifications()->size() == 64);
  for (int i = 0; i < 65; ++i) auth->Publish(delivery);
  Check(!first.Identity() && !first.TakeNotifications());
  Check(other.Identity().has_value());
  auto replacement = auth->Consume(auth->Issue(recipient, u"Recipient"));
  Check(replacement.Identity().has_value() &&
        replacement.TakeNotifications()->empty());
  auth->Publish(delivery);
  auto newer = auth->Consume(auth->Issue(recipient, u"Recipient"));
  Check(!replacement.Identity() && newer.TakeNotifications()->empty());
  recipient.reset();
  Check(!newer.Identity() && !newer.TakeNotifications());
  auth->Publish(delivery);  // Offline recipients are not retained.
  recipient = auth->BindGame({2, 20});
  auto fresh = auth->Consume(auth->Issue(recipient, u"Recipient"));
  Check(fresh.TakeNotifications()->empty());
}
void Sessions() {
  const std::array<proto::ClanPacket, 6> requests{
      proto::ClanJoinRequest{1000, u"Clan"}.Encode(),
      proto::ClanAutoJoinRequest{1000, u"Clan", u"Actor"}.Encode(),
      proto::ClanCancelJoinRequest{1000}.Encode(),
      proto::ClanLeaveRequest{}.Encode(),
      proto::ClanMemberChangeRequest{20, u"Target", 1, 100}.Encode(),
      proto::ClanRecruitmentRequest{1000, 2}.Encode()};
  for (std::size_t i = 0; i < requests.size(); ++i) {
    auto auth = std::make_shared<Auth>();
    auto actor = auth->BindGame({1, 10});
    auto target = auth->BindGame({2, 20});
    Session recipient(auth, Profiles);
    recipient.Process(
        proto::ClanLoginRequest{auth->Issue(target, u"Target")}.Encode());
    int calls = 0;
    const auto event = proto::ClanApplicationChangedNotification{}.Encode();
    Session session(
        auth, Profiles, Auth::Clock::now, {}, {}, {},
        [&](const auto& identity, const clans::ClanMutation& mutation,
            const auto& authorized) {
          ++calls;
          Check(identity == clans::ClanIdentity{1, 10});
          Check(mutation.index() == i && authorized());
          return clans::ClanMutationResult{
              {proto::ClanRecruitmentResponse{0, 1, 2}.Encode()},
              {{{20},
                std::make_shared<const std::vector<proto::ClanPacket>>(
                    std::vector{event})}}};
        });
    session.Process(
        proto::ClanLoginRequest{auth->Issue(actor, u"Actor")}.Encode());
    Check(session.Process(requests[i]).responses[0].message_id == 1103 &&
          calls == 1);
    // A packet arriving before the timer drains older events before its reply.
    const auto replies =
        recipient.Process(proto::ClanProfileRequest{}.Encode()).responses;
    Check(replies.size() == 2 && replies[0] == event &&
          replies[1].message_id == 1016);
    Check(recipient.Poll().responses.empty());
    auto malformed = requests[i];
    malformed.payload.push_back(0);
    Rejects([&] { session.Process(malformed); });
    Check(calls == 1 && session.Poll().close_connection);
    Session unavailable(auth, Profiles);
    unavailable.Process(
        proto::ClanLoginRequest{auth->Issue(actor, u"Actor")}.Encode());
    Rejects([&] { unavailable.Process(requests[i]); });
    Check(unavailable.Poll().close_connection);
    Session early(auth, Profiles);
    Rejects([&] { early.Process(requests[i]); });
  }
  auto auth = std::make_shared<Auth>();
  auto game = auth->BindGame({1, 10});
  Session session(auth, Profiles, Auth::Clock::now, {}, {}, {},
                  [&](const auto&, const auto&, const auto& authorized) {
                    Check(authorized());
                    game.reset();
                    Check(!authorized());
                    return clans::ClanMutationResult{
                        {proto::ClanRecruitmentResponse{0, 1, 2}.Encode()}, {}};
                  });
  session.Process(
      proto::ClanLoginRequest{auth->Issue(game, u"Actor")}.Encode());
  const auto revoked =
      session.Process(proto::ClanRecruitmentRequest{1000, 2}.Encode());
  Check(revoked.close_connection && revoked.responses.empty());
}
void Serialization() {
  auto auth = std::make_shared<Auth>();
  auto a = auth->BindGame({1, 10});
  auto b = auth->BindGame({2, 20});
  std::atomic<int> active = 0, peak = 0, calls = 0;
  const auto mutate = [&](const auto&, const auto&, const auto&) {
    const auto count = ++active;
    int old = peak.load();
    while (old < count && !peak.compare_exchange_weak(old, count)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    --active;
    ++calls;
    return clans::ClanMutationResult{
        {proto::ClanRecruitmentResponse{0, 1, 1}.Encode()}, {}};
  };
  Session first(auth, Profiles, Auth::Clock::now, {}, {}, {}, mutate),
      second(auth, Profiles, Auth::Clock::now, {}, {}, {}, mutate);
  first.Process(proto::ClanLoginRequest{auth->Issue(a, u"Actor")}.Encode());
  second.Process(proto::ClanLoginRequest{auth->Issue(b, u"Target")}.Encode());
  std::barrier gate(2);
  auto run = [&](Session& s) {
    gate.arrive_and_wait();
    for (int i = 0; i < 8; ++i)
      s.Process(proto::ClanRecruitmentRequest{1000, 1}.Encode());
  };
  std::jthread one([&] { run(first); }), two([&] { run(second); });
  one.join();
  two.join();
  Check(peak == 1 && calls == 16 && active == 0);
}
}  // namespace

int RunClanMutationTests(const std::filesystem::path& directory) {
  Reports(directory);
  Queues();
  Sessions();
  Serialization();
  std::cout << "Clan mutation and report: " << checks << " checks passed\n";
  return checks;
}
