#include "sessions/connection_handoff.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "features/authentication/credential_crypto.h"
#include "protocol/scenes/channel_transfer.h"
#include "protocol/scenes/move_into_response.h"

namespace bubble_fighter::server::sessions {
using features::authentication::CredentialCrypto;
using network::PacketHandlingResult;
using namespace protocol;
using features::channels::ChannelMovePhase;
using features::channels::ChannelMoveSignal;
using features::channels::ChannelMoveStatus;

struct ConnectionHandoffs::Context {
  enum class Phase {
    kIdle,
    kOffered,
    kClaimed,
    kEntered,
    kSwitchSent,
    kAcknowledged
  };
  std::mutex mutex;
  network::ConnectionHandlers handlers;
  HandoffSessionHooks hooks;
  std::uint64_t owner = 0;
  std::uint64_t claimant = 0;
  std::uint32_t destination_category = 60;
  bool capable = false;
  bool closed = false;
  bool cancellation_notice = false;
  std::optional<std::uint32_t> group_refusal;
  Phase phase = Phase::kIdle;
  Clock::time_point deadline{};
  HandoffSecret digest{};
  std::optional<HandoffIdentity> subject;
};

struct ConnectionHandoffs::Connection {
  std::shared_ptr<ConnectionHandoffs> registry;
  std::shared_ptr<Context> context;
  std::uint64_t id = 0;
  bool fresh = true;  // Proof must be the first application packet.
  bool disconnected = false;

  ~Connection() { Disconnect(); }
  void RemoveGrant() {
    std::lock_guard lock(registry->registry_mutex_);
    auto found = registry->grants_.find(context->digest);
    if (found != registry->grants_.end() && found->second.lock() == context)
      registry->grants_.erase(found);
  }
  bool Switched() const {
    return context->phase == Context::Phase::kSwitchSent ||
           context->phase == Context::Phase::kAcknowledged ||
           (context->hooks.group && context->hooks.group->irreversible());
  }
  void CloseContext() noexcept {
    if (context->hooks.group) {
      try {
        context->hooks.group->cancel();
      } catch (...) {
      }
    }
    context->closed = true;
    context->owner = 0;
    context->claimant = 0;
    context->handlers = {};
    context->hooks = {};
  }
  void Cancel() {
    RemoveGrant();
    if (context->hooks.group) context->hooks.group->cancel();
    context->hooks.resume();
    context->phase = Context::Phase::kIdle;
    context->claimant = 0;
    context->subject.reset();
    context->cancellation_notice = true;
  }
  void Expire() {
    if (!context->closed && context->phase != Context::Phase::kIdle &&
        registry->now_() >= context->deadline) {
      if (Switched()) {
        RemoveGrant();
        CloseContext();  // Client may already have promoted the new socket.
      } else {
        Cancel();
      }
    }
  }
  void Disconnect() noexcept {
    if (disconnected || !context) return;
    disconnected = true;
    std::lock_guard lock(context->mutex);
    if (context->closed) return;
    try {
      if (context->owner == id || (context->claimant == id && Switched())) {
        RemoveGrant();
        CloseContext();
      } else if (context->claimant == id) {
        Cancel();
      }
    } catch (...) {
      CloseContext();
    }
  }
  bool Authenticated() {
    std::lock_guard lock(context->mutex);
    return !disconnected && !context->closed && context->owner == id &&
           context->hooks.identity().has_value();
  }
  PacketHandlingResult Claim(const TcpPacket& packet) {
    if (!fresh)
      throw std::runtime_error("Handoff proof must be the first packet.");
    fresh = false;
    const auto proof = HandoffProofRequest::Decode(packet);
    const auto digest = CredentialCrypto::Sha256(proof.secret);
    std::shared_ptr<Context> source;
    {
      std::lock_guard lock(registry->registry_mutex_);
      const auto found = registry->grants_.find(digest);
      if (found != registry->grants_.end()) {
        source = found->second.lock();
        registry->grants_.erase(
            found);  // One attempt, never reissued on rollback.
      }
    }
    if (!source || source == context)
      throw std::runtime_error("Handoff proof rejected.");
    std::lock_guard lock(source->mutex);
    if (source->closed || source->phase != Context::Phase::kOffered ||
        registry->now_() >= source->deadline || !source->subject ||
        source->subject->character_id != proof.character_id ||
        source->hooks.identity() != source->subject)
      throw std::runtime_error("Handoff proof rejected.");
    // The fresh unauthenticated session is discarded. All existing leases and
    // packet queues remain in the source session, serialized by its mutex.
    context = std::move(source);
    context->claimant = id;
    context->phase = Context::Phase::kClaimed;
    return {};
  }
  std::optional<PacketHandlingResult> Offer(const TcpPacket& packet) {
    if (!context->capable ||
        packet.message_id != MoveChannelRequest::kMessageId)
      return std::nullopt;
    const auto request = MoveChannelRequest::Decode(packet);
    const auto identity = context->hooks.identity();
    if (!identity || !identity->account_id || !identity->character_id)
      return std::nullopt;
    std::array<std::uint8_t, 4> address;
    std::uint16_t port;
    HandoffSecret secret;
    {
      std::lock_guard lock(registry->registry_mutex_);
      address = registry->address_;
      port = registry->port_;
      if (!port || address == std::array<std::uint8_t, 4>{})
        return std::nullopt;
      secret = registry->secrets_();
    }
    if (std::all_of(secret.begin(), secret.end(),
                    [](auto b) { return b == 0; }))
      throw std::runtime_error("Invalid handoff secret source.");
    const auto digest = CredentialCrypto::Sha256(secret);
    PacketHandlingResult result{
        {HandoffGrantNotification{identity->character_id,
                                  request.channel_category, secret}
             .Encode(),
         MoveChannelResponse{0, address, port, request.channel_category}
             .Encode()},
        false};
    secret.fill(0);
    const auto deadline = registry->now_() + std::chrono::seconds(10);
    if (!context->hooks.prepare(request.channel_category)) {
      if (context->hooks.refusal)
        return PacketHandlingResult{
            {MoveChannelResponse{
                context->hooks.refusal(), {}, 0, request.channel_category}
                 .Encode()},
            false};
      return std::nullopt;
    }
    try {
      std::lock_guard lock(registry->registry_mutex_);
      std::erase_if(registry->grants_,
                    [](const auto& row) { return row.second.expired(); });
      if (registry->grants_.size() >= 64 ||
          !registry->grants_.emplace(digest, context).second)
        throw std::runtime_error("Handoff grant capacity or collision.");
    } catch (...) {
      context->hooks.resume();
      throw;
    }
    context->digest = digest;
    context->subject = identity;
    context->destination_category = request.channel_category;
    context->deadline = deadline;
    context->phase = Context::Phase::kOffered;
    return result;
  }
  PacketHandlingResult Process(const TcpPacket& packet) {
    if (disconnected) return {{}, true};
    if (packet.message_id == HandoffProofRequest::kMessageId)
      return Claim(packet);
    fresh = false;
    std::lock_guard lock(context->mutex);
    Expire();
    auto group = ObserveGroup();
    if (context->closed) return {{}, true};
    if (context->claimant == id) {
      if (context->hooks.identity() != context->subject)
        throw std::runtime_error("Handoff source identity changed.");
      if (context->phase == Context::Phase::kClaimed &&
          packet.message_id == MoveIntoRequest::kMessageId) {
        const auto request = MoveIntoRequest::Decode(packet);
        if (request.character_id != context->subject->character_id ||
            request.unknown_ui_value > 1)
          throw std::runtime_error("Handoff native entry rejected.");
        if (context->hooks.group && group.phase != ChannelMovePhase::kIdle) {
          context->hooks.group->signal(ChannelMoveSignal::kEntered);
          context->phase = Context::Phase::kEntered;
          return AdvanceGroup(ObserveGroup());
        }
        PacketHandlingResult result{
            {ChannelDisconnectingNotification{}.Encode()}, false};
        context->phase = Context::Phase::kSwitchSent;
        return result;
      }
      if (context->phase == Context::Phase::kSwitchSent &&
          packet.message_id ==
              ChannelSwitchAcknowledgementRequest::kMessageId) {
        ChannelSwitchAcknowledgementRequest::Decode(packet);
        if (context->hooks.group && group.phase != ChannelMovePhase::kIdle) {
          context->hooks.group->signal(ChannelMoveSignal::kAcknowledged);
          context->phase = Context::Phase::kAcknowledged;
          return AdvanceGroup(ObserveGroup());
        }
        return Complete();
      }
      throw std::runtime_error("Unexpected handoff entry sequence.");
    }
    if (context->owner != id) return {{}, true};
    if (Switched()) return {};
    if (context->phase != Context::Phase::kIdle ||
        group.phase != ChannelMovePhase::kIdle)
      Cancel();
    std::vector<TcpPacket> prefix;
    if (context->cancellation_notice) {
      prefix.push_back(HandoffCancelledNotification{}.Encode());
      context->cancellation_notice = false;
    }
    if (context->group_refusal) {
      prefix.push_back(
          MoveChannelResponse{2, {}, 0, *context->group_refusal}.Encode());
      context->group_refusal.reset();
    }
    PacketHandlingResult result;
    if (packet.message_id == HandoffCapabilityRequest::kMessageId) {
      HandoffCapabilityRequest::Decode(packet);
      if (!context->hooks.identity())
        throw std::runtime_error("Handoff capability before authentication.");
      context->capable = true;
      if (context->hooks.group) context->hooks.group->enable();
    } else if (const auto began = BeginGroup(packet)) {
      if (!*began)
        result.responses.push_back(MoveChannelResponse{
            context->hooks.refusal ? context->hooks.refusal() : 2U,
            {},
            0,
            MoveChannelRequest::Decode(packet).channel_category}
                                       .Encode());
    } else if (auto offer = Offer(packet)) {
      result = std::move(*offer);
    } else {
      result = context->handlers.on_packet(packet);
    }
    prefix.reserve(prefix.size() + result.responses.size());
    for (auto& response : result.responses)
      prefix.push_back(std::move(response));
    result.responses = std::move(prefix);
    return result;
  }
  std::optional<bool> BeginGroup(const TcpPacket& packet) {
    if (!context->capable || !context->hooks.group ||
        packet.message_id != MoveChannelRequest::kMessageId)
      return {};
    return context->hooks.group->begin(
        MoveChannelRequest::Decode(packet).channel_category);
  }
  ChannelMoveStatus ObserveGroup() {
    if (context->closed || !context->hooks.group) return {};
    auto group = context->hooks.group->poll();
    if (group.phase == ChannelMovePhase::kClosed) {
      RemoveGrant();
      CloseContext();
    } else if (group.phase == ChannelMovePhase::kCancelled) {
      const bool offered = context->phase != Context::Phase::kIdle;
      Cancel();
      context->cancellation_notice = offered;
      if (group.initiator) context->group_refusal = group.category;
      return {};
    }
    return group;
  }
  PacketHandlingResult Complete() {
    PacketHandlingResult result{
        {ChannelDisconnectNotification{}.Encode(),
         MoveIntoResponse{1, context->destination_category, 0}.Encode()},
        false};
    if (context->hooks.commit) {
      auto scene = context->hooks.commit();
      for (auto& response : scene)
        result.responses.push_back(std::move(response));
    } else
      context->hooks.resume();
    context->owner = id;
    context->claimant = 0;
    context->phase = Context::Phase::kIdle;
    context->subject.reset();
    return result;
  }
  PacketHandlingResult AdvanceGroup(const ChannelMoveStatus& group) {
    if (context->closed) return {{}, true};
    if (context->claimant != id) return {};
    if (context->phase == Context::Phase::kEntered &&
        group.phase == ChannelMovePhase::kSwitch) {
      auto packet = ChannelDisconnectingNotification{}.Encode();
      if (!context->hooks.group->signal(ChannelMoveSignal::kSwitched))
        return {};
      context->phase = Context::Phase::kSwitchSent;
      return {{std::move(packet)}, false};
    }
    if (context->phase == Context::Phase::kAcknowledged &&
        group.phase == ChannelMovePhase::kCommitted)
      return Complete();
    return {};
  }
  PacketHandlingResult Poll() {
    std::lock_guard lock(context->mutex);
    Expire();
    const auto group = ObserveGroup();
    if (disconnected || context->closed ||
        (context->owner != id && context->claimant != id))
      return {{}, true};
    if (context->claimant == id) return AdvanceGroup(group);
    if (group.phase == ChannelMovePhase::kOffer &&
        context->phase == Context::Phase::kIdle) {
      if (auto offered = Offer(MoveChannelRequest{group.category}.Encode()))
        return std::move(*offered);
      Cancel();
      context->group_refusal = group.category;
    }
    if (context->phase != Context::Phase::kIdle) return {};
    PacketHandlingResult result;
    if (context->cancellation_notice) {
      result.responses.push_back(HandoffCancelledNotification{}.Encode());
      context->cancellation_notice = false;
    }
    if (context->group_refusal) {
      result.responses.push_back(
          MoveChannelResponse{2, {}, 0, *context->group_refusal}.Encode());
      context->group_refusal.reset();
    }
    if (group.phase != ChannelMovePhase::kIdle) return result;
    if (context->handlers.on_tick) {
      auto tick = context->handlers.on_tick();
      for (auto& p : tick.responses) result.responses.push_back(std::move(p));
      result.close_connection = tick.close_connection;
    }
    return result;
  }
};

ConnectionHandoffs::ConnectionHandoffs(Now now, SecretFactory secrets)
    : now_(std::move(now)), secrets_(std::move(secrets)) {
  if (!now_) throw std::invalid_argument("Missing handoff clock.");
  if (!secrets_) secrets_ = CredentialCrypto::GenerateSecret;
}
void ConnectionHandoffs::SetEndpoint(std::array<std::uint8_t, 4> address,
                                     std::uint16_t port) {
  std::lock_guard lock(registry_mutex_);
  address_ = address;
  port_ = port;
}
HandoffConnectionHandlers ConnectionHandoffs::Attach(
    network::ConnectionHandlers handlers, HandoffSessionHooks hooks) {
  if (!handlers.on_packet || !hooks.identity || !hooks.prepare || !hooks.resume)
    throw std::invalid_argument("Incomplete handoff session hooks.");
  if (hooks.group && (!hooks.group->enable || !hooks.group->begin ||
                      !hooks.group->poll || !hooks.group->signal ||
                      !hooks.group->cancel || !hooks.group->irreversible))
    throw std::invalid_argument("Incomplete handoff session hooks.");
  auto context = std::make_shared<Context>();
  context->handlers = std::move(handlers);
  context->hooks = std::move(hooks);
  auto connection = std::make_shared<Connection>();
  connection->registry = shared_from_this();
  connection->context = context;
  {
    std::lock_guard lock(registry_mutex_);
    if (next_connection_ == std::numeric_limits<std::uint64_t>::max())
      throw std::runtime_error("Handoff connection identities exhausted.");
    connection->id = context->owner = next_connection_++;
  }
  HandoffConnectionHandlers result;
  result.handlers.on_packet = [connection](const TcpPacket& packet) {
    try {
      return connection->Process(packet);
    } catch (...) {
      connection->Disconnect();
      throw;
    }
  };
  result.handlers.on_tick = [connection] {
    try {
      return connection->Poll();
    } catch (...) {
      connection->Disconnect();
      throw;
    }
  };
  result.authenticated = [connection] { return connection->Authenticated(); };
  return result;
}
}  // namespace bubble_fighter::server::sessions
