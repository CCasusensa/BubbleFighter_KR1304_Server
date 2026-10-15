#include "sessions/connection_handoff.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "protocol/scenes/channel_transfer.h"
#include "protocol/scenes/handoff_messages.h"
#include "protocol/scenes/move_into_response.h"

namespace bf = bubble_fighter::server;
namespace proto = bf::protocol;
namespace sessions = bf::sessions;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Connection handoff line " +
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
void Ids(const bf::network::PacketHandlingResult& result,
         std::initializer_list<std::uint16_t> ids) {
  Check(!result.close_connection && result.responses.size() == ids.size());
  Check(std::equal(result.responses.begin(), result.responses.end(),
                   ids.begin(),
                   [](const auto& p, auto id) { return p.message_id == id; }));
}
struct Domain {
  bool authenticated = true;
  bool prepared = false;
  bool allowed = true;
  bool fail_resume = false;
  int calls = 0;
  std::vector<proto::TcpPacket> queued;
};
struct Fixture {
  sessions::ConnectionHandoffs::Clock::time_point now{};
  std::uint8_t next_secret = 1;
  std::shared_ptr<sessions::ConnectionHandoffs> registry =
      std::make_shared<sessions::ConnectionHandoffs>(
          [this] { return now; },
          [this] {
            proto::HandoffSecret secret{};
            secret.fill(next_secret++);
            return secret;
          });
  std::shared_ptr<Domain> domain = std::make_shared<Domain>();
  sessions::HandoffConnectionHandlers source = Attach(domain);
  Fixture() { registry->SetEndpoint({127, 0, 0, 1}, 17110); }
  sessions::HandoffConnectionHandlers Attach(
      std::shared_ptr<Domain> state = {}) {
    if (!state) {
      state = std::make_shared<Domain>();
      state->authenticated = false;
    }
    bf::network::ConnectionHandlers handlers;
    handlers.on_packet = [state](const auto& packet) {
      ++state->calls;
      if (packet.message_id == 53) {
        return bf::network::PacketHandlingResult{
            {proto::MoveChannelResponse{
                2,
                {},
                0,
                proto::MoveChannelRequest::Decode(packet).channel_category}
                 .Encode()},
            false};
      }
      if (packet.message_id == 54 || packet.message_id == 55)
        throw std::runtime_error("Unapproved native handoff");
      return bf::network::PacketHandlingResult{{{200, {7}}}, false};
    };
    handlers.on_tick = [state] {
      auto result = std::move(state->queued);
      state->queued.clear();
      return bf::network::PacketHandlingResult{std::move(result), false};
    };
    return registry->Attach(
        std::move(handlers),
        {[state]() -> std::optional<sessions::HandoffIdentity> {
           return state->authenticated
                      ? std::optional{sessions::HandoffIdentity{7, 17}}
                      : std::nullopt;
         },
         [state](std::uint32_t category) {
           if (category != 60 || !state->allowed) return false;
           state->prepared = true;
           return true;
         },
         [state] {
           if (state->fail_resume) throw std::runtime_error("Resume failed");
           state->prepared = false;
         }});
  }
  proto::HandoffGrantNotification Offer() {
    Ids(source.handlers.on_packet(proto::HandoffCapabilityRequest{}.Encode()),
        {});
    auto offered =
        source.handlers.on_packet(proto::MoveChannelRequest{60}.Encode());
    Ids(offered, {65021, 8});
    auto grant = proto::HandoffGrantNotification::Decode(offered.responses[0]);
    Check(grant.character_id == 17 && grant.channel_category == 60 &&
          std::any_of(grant.secret.begin(), grant.secret.end(),
                      [](auto b) { return b != 0; }));
    Check(proto::MoveChannelResponse::Decode(offered.responses[1]) ==
          proto::MoveChannelResponse{0, {127, 0, 0, 1}, 17110, 60});
    Check(domain->prepared && source.authenticated());
    return grant;
  }
  static proto::TcpPacket Proof(const proto::HandoffGrantNotification& grant) {
    return proto::HandoffProofRequest{grant.character_id, grant.secret}
        .Encode();
  }
  static proto::TcpPacket Entry() {
    return proto::MoveIntoRequest{17, 1}.Encode();
  }
  static proto::TcpPacket Ack() {
    return proto::ChannelSwitchAcknowledgementRequest{}.Encode();
  }
};

void CodecBounds() {
  proto::HandoffSecret secret;
  for (std::size_t i = 0; i < secret.size(); ++i)
    secret[i] = static_cast<std::uint8_t>(i + 1);
  // Fixed companion-v1 byte layout, defined by this project before server use.
  auto grant = proto::HandoffGrantNotification{0x12345678, 60, secret}.Encode();
  std::vector<std::uint8_t> expected{1,    0,    0,  0, 0x78, 0x56,
                                     0x34, 0x12, 60, 0, 0,    0};
  expected.insert(expected.end(), secret.begin(), secret.end());
  Check(grant.payload == expected);
  auto proof = proto::HandoffProofRequest{0x12345678, secret}.Encode();
  expected.erase(expected.begin() + 8, expected.begin() + 12);
  Check(proof.payload == expected);
  auto decode = [](const proto::TcpPacket& p) {
    if (p.message_id == 65020)
      return proto::HandoffCapabilityRequest::Decode(p).Encode();
    if (p.message_id == 65021)
      return proto::HandoffGrantNotification::Decode(p).Encode();
    if (p.message_id == 65022)
      return proto::HandoffProofRequest::Decode(p).Encode();
    return proto::HandoffCancelledNotification::Decode(p).Encode();
  };
  for (const auto& packet :
       {grant, proof, proto::HandoffCapabilityRequest{}.Encode(),
        proto::HandoffCancelledNotification{}.Encode()}) {
    Check(decode(packet).payload == packet.payload);
    for (std::size_t n = 0; n < packet.payload.size(); ++n) {
      auto p = packet;
      p.payload.resize(n);
      Rejects([&] { decode(p); });
    }
    auto p = packet;
    p.payload.push_back(0);
    Rejects([&] { decode(p); });
    p = packet;
    p.payload[0] = 2;
    Rejects([&] { decode(p); });
  }
  Rejects(
      [] { proto::HandoffCapabilityRequest::Decode({65019, {1, 0, 0, 0}}); });
  Rejects(
      [] { proto::HandoffGrantNotification::Decode({65020, {1, 0, 0, 0}}); });
  Rejects([] { proto::HandoffProofRequest::Decode({65021, {1, 0, 0, 0}}); });
  Rejects([] {
    proto::HandoffCancelledNotification::Decode({65022, {1, 0, 0, 0}});
  });
}
void SuccessfulOwnership() {
  Fixture f;
  Ids(f.source.handlers.on_packet(proto::MoveChannelRequest{60}.Encode()), {8});
  auto grant = f.Offer();
  f.domain->queued.push_back({503, {42}});
  Ids(f.source.handlers.on_tick(),
      {});  // Source must not drain the future owner's queue.
  auto destination = f.Attach();
  Ids(destination.handlers.on_packet(Fixture::Proof(grant)), {});
  Check(!destination.authenticated());
  auto replay = f.Attach();
  Rejects([&] { replay.handlers.on_packet(Fixture::Proof(grant)); });
  Ids(destination.handlers.on_packet(Fixture::Entry()), {9});
  const auto calls = f.domain->calls;
  Ids(f.source.handlers.on_packet({100, {}}), {});
  Check(f.domain->calls == calls);
  auto committed = destination.handlers.on_packet(Fixture::Ack());
  Ids(committed, {10, 11});
  Check(proto::MoveIntoResponse::Decode(committed.responses[1]).IsAccepted());
  Check(!f.domain->prepared && destination.authenticated() &&
        !f.source.authenticated());
  Check(f.source.handlers.on_packet({100, {}}).close_connection);
  Check(f.source.handlers.on_tick().close_connection);
  f.source =
      {};  // Retired source destruction cannot delete the new owner's session.
  auto queued = destination.handlers.on_tick();
  Ids(queued, {503});
  Check(queued.responses[0].payload == std::vector<std::uint8_t>{42});
  Ids(destination.handlers.on_packet({100, {}}), {200});
  Check(f.domain->calls == calls + 1);
  f.source = std::move(destination);
  auto second = f.Offer();
  Check(second.secret != grant.secret);
  auto next = f.Attach();
  Ids(next.handlers.on_packet(Fixture::Proof(second)), {});
  Ids(next.handlers.on_packet(Fixture::Entry()), {9});
  Ids(next.handlers.on_packet(Fixture::Ack()), {10, 11});
  Check(next.authenticated() && !f.source.authenticated());
}
void RollbackAndClosure() {
  for (int stage = 0; stage < 3; ++stage) {
    Fixture f;
    auto grant = f.Offer();
    auto next = f.Attach();
    if (stage >= 1) Ids(next.handlers.on_packet(Fixture::Proof(grant)), {});
    if (stage >= 2) Ids(next.handlers.on_packet(Fixture::Entry()), {9});
    f.now += std::chrono::seconds(10);
    const auto result = f.source.handlers.on_tick();
    if (stage < 2) {
      Ids(result, {65023});
      Check(f.source.authenticated() && !f.domain->prepared);
      Ids(f.source.handlers.on_packet({100, {}}), {200});
    } else {
      Check(result.close_connection && !f.source.authenticated());
    }
    if (stage) Check(next.handlers.on_tick().close_connection);
    auto replay = f.Attach();
    Rejects([&] { replay.handlers.on_packet(Fixture::Proof(grant)); });
  }
  for (bool switched : {false, true}) {
    Fixture f;
    auto grant = f.Offer();
    auto next = f.Attach();
    next.handlers.on_packet(Fixture::Proof(grant));
    if (switched) next.handlers.on_packet(Fixture::Entry());
    next = {};
    if (switched)
      Check(f.source.handlers.on_tick().close_connection);
    else {
      Ids(f.source.handlers.on_tick(), {65023});
      Check(!f.domain->prepared);
    }
  }
  for (int stage = 0; stage < 3; ++stage) {
    Fixture f;
    auto grant = f.Offer();
    auto next = f.Attach();
    if (stage >= 1) next.handlers.on_packet(Fixture::Proof(grant));
    if (stage >= 2) next.handlers.on_packet(Fixture::Entry());
    f.source = {};
    if (stage == 0)
      Rejects([&] { next.handlers.on_packet(Fixture::Proof(grant)); });
    else
      Check(next.handlers.on_tick().close_connection && !next.authenticated());
  }
  for (const auto& invalid :
       {Fixture::Ack(), proto::MoveIntoRequest{18, 1}.Encode(),
        proto::MoveIntoRequest{17, 2}.Encode(), proto::TcpPacket{54, {}}}) {
    Fixture f;
    auto grant = f.Offer();
    auto next = f.Attach();
    next.handlers.on_packet(Fixture::Proof(grant));
    Rejects([&] { next.handlers.on_packet(invalid); });
    Ids(f.source.handlers.on_tick(), {65023});
    Check(f.source.authenticated());
  }
  {
    Fixture f;
    auto grant = f.Offer();
    auto next = f.Attach();
    next.handlers.on_packet(Fixture::Proof(grant));
    Ids(f.source.handlers.on_packet({100, {}}), {65023, 200});
    Check(next.handlers.on_packet(Fixture::Entry()).close_connection);
    Check(!f.domain->prepared && f.source.authenticated());
  }
  {
    Fixture f;
    auto grant = f.Offer();
    auto next = f.Attach();
    next.handlers.on_packet(Fixture::Proof(grant));
    next.handlers.on_packet(Fixture::Entry());
    f.domain->fail_resume = true;
    Rejects([&] { next.handlers.on_packet(Fixture::Ack()); });
    Check(f.source.handlers.on_tick().close_connection &&
          !next.authenticated());
  }
}
void InvalidAuthority() {
  Fixture f;
  auto unauthenticated = f.Attach();
  Rejects([&] {
    unauthenticated.handlers.on_packet(
        proto::HandoffCapabilityRequest{}.Encode());
  });
  auto grant = f.Offer();
  auto wrong = grant;
  wrong.secret[7] ^= 1;
  auto attacker = f.Attach();
  Rejects([&] { attacker.handlers.on_packet(Fixture::Proof(wrong)); });
  Check(f.source.authenticated());
  auto native_only = f.Attach();
  Rejects([&] { native_only.handlers.on_packet(Fixture::Entry()); });
  auto late = f.Attach();
  late.handlers.on_packet({1, {}});
  Rejects([&] { late.handlers.on_packet(Fixture::Proof(grant)); });
  auto valid = f.Attach();
  Ids(valid.handlers.on_packet(Fixture::Proof(grant)), {});
  valid = {};
  Ids(f.source.handlers.on_tick(), {65023});
  f.domain->allowed = false;
  Ids(f.source.handlers.on_packet(proto::MoveChannelRequest{60}.Encode()), {8});
  Check(!f.domain->prepared);
  f.domain->allowed = true;
  for (auto category : {20U, 70U, 80U, 0xffffffffU})
    Ids(f.source.handlers.on_packet(
            proto::MoveChannelRequest{category}.Encode()),
        {8});
  f.registry->SetEndpoint({}, 17110);
  Ids(f.source.handlers.on_packet(proto::MoveChannelRequest{60}.Encode()), {8});
}
void CommitDisconnectRace() {
  for (int round = 0; round < 12; ++round) {
    Fixture f;
    auto grant = f.Offer();
    auto next = f.Attach();
    next.handlers.on_packet(Fixture::Proof(grant));
    next.handlers.on_packet(Fixture::Entry());
    std::barrier gate(3);
    bf::network::PacketHandlingResult result;
    std::thread commit([&] {
      gate.arrive_and_wait();
      result = next.handlers.on_packet(Fixture::Ack());
    });
    std::thread disconnect([&] {
      gate.arrive_and_wait();
      f.source = {};
    });
    gate.arrive_and_wait();
    commit.join();
    disconnect.join();
    if (result.close_connection) {
      Check(!next.authenticated() && next.handlers.on_tick().close_connection);
    } else {
      Check(next.authenticated() && result.responses.size() == 2 &&
            result.responses[0].message_id == 10 &&
            result.responses[1].message_id == 11 &&
            next.handlers.on_packet({100, {}}).responses[0].message_id == 200);
    }
  }
  Fixture f;
  auto grant = f.Offer();
  auto wrong = f.Attach();
  auto proof = proto::HandoffProofRequest{18, grant.secret}.Encode();
  Rejects([&] { wrong.handlers.on_packet(proof); });
  auto retry = f.Attach();
  Rejects([&] { retry.handlers.on_packet(Fixture::Proof(grant)); });
  f.now += std::chrono::seconds(10);
  Ids(f.source.handlers.on_tick(), {65023});
  Check(f.source.authenticated() && !f.domain->prepared);
}
void CompetingClaims() {
  for (int round = 0; round < 12; ++round) {
    Fixture f;
    auto grant = f.Offer();
    std::array peers{f.Attach(), f.Attach()};
    std::barrier gate(3);
    std::array<bool, 2> claimed{};
    std::array<std::thread, 2> workers;
    for (std::size_t i = 0; i < 2; ++i)
      workers[i] = std::thread([&, i] {
        gate.arrive_and_wait();
        try {
          peers[i].handlers.on_packet(Fixture::Proof(grant));
          claimed[i] = true;
        } catch (const std::exception&) {
        }
      });
    gate.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    Check(claimed[0] != claimed[1]);
    const auto winner = claimed[0] ? 0U : 1U;
    Ids(peers[winner].handlers.on_packet(Fixture::Entry()), {9});
    Ids(peers[winner].handlers.on_packet(Fixture::Ack()), {10, 11});
    Check(peers[winner].authenticated() && !f.source.authenticated());
    Check(!peers[1U - winner].authenticated());
  }
}
}  // namespace
int RunConnectionHandoffTests() {
  CodecBounds();
  SuccessfulOwnership();
  RollbackAndClosure();
  InvalidAuthority();
  CompetingClaims();
  CommitDisconnectRace();
  std::cout << "Connection handoff: " << checks << " checks passed\n";
  return checks;
}
