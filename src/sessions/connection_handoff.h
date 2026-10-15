#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_CONNECTION_HANDOFF_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_CONNECTION_HANDOFF_H_
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "features/channels/party_channel_transfers.h"
#include "network/tcp_server.h"
#include "protocol/scenes/handoff_messages.h"

namespace bubble_fighter::server::sessions {
struct HandoffIdentity {
  std::uint64_t account_id = 0;
  std::uint32_t character_id = 0;
  bool operator==(const HandoffIdentity&) const = default;
};
struct HandoffSessionHooks {
  std::function<std::optional<HandoffIdentity>()> identity;
  // Called while serialized with session packet/tick handlers. Prepare must
  // leave the source intact on false/throw. Resume may publish busy changes.
  std::function<bool(std::uint32_t)> prepare;
  std::function<void()> resume;
  std::function<std::vector<protocol::TcpPacket>()> commit;
  struct Group {
    std::function<void()> enable;
    std::function<std::optional<bool>(std::uint32_t)> begin;
    std::function<features::channels::ChannelMoveStatus()> poll;
    std::function<bool(features::channels::ChannelMoveSignal)> signal;
    std::function<void()> cancel;
    std::function<bool()> irreversible;
  };
  std::shared_ptr<Group> group;
  std::function<std::uint32_t()> refusal;
};
struct HandoffConnectionHandlers {
  network::ConnectionHandlers handlers;
  std::function<bool()> authenticated;
};

// Rebinds the SAME authenticated session to a fresh cipher/socket connection.
// Channel category changes and whole-party moves are separate operations.
class ConnectionHandoffs
    : public std::enable_shared_from_this<ConnectionHandoffs> {
 public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;
  using SecretFactory = std::function<protocol::HandoffSecret()>;
  explicit ConnectionHandoffs(Now now = Clock::now, SecretFactory secrets = {});
  void SetEndpoint(std::array<std::uint8_t, 4> address, std::uint16_t port);
  HandoffConnectionHandlers Attach(network::ConnectionHandlers handlers,
                                   HandoffSessionHooks hooks);

 private:
  struct Context;
  struct Connection;
  friend struct Connection;
  // No registry lock is held while acquiring a context lock. Normal session
  // handlers on different accounts remain concurrent, including database work.
  std::mutex registry_mutex_;
  std::map<protocol::HandoffSecret, std::weak_ptr<Context>> grants_;
  std::array<std::uint8_t, 4> address_{};
  std::uint16_t port_ = 0;
  std::uint64_t next_connection_ = 1;
  Now now_;
  SecretFactory secrets_;
};
}  // namespace bubble_fighter::server::sessions
#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_CONNECTION_HANDOFF_H_
