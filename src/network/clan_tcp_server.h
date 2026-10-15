#ifndef BUBBLEFIGHTER_SERVER_NETWORK_CLAN_TCP_SERVER_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_CLAN_TCP_SERVER_H_

#include <functional>
#include <stop_token>
#include <string_view>

#include "protocol/clans/clan_packet.h"

namespace bubble_fighter::server::network {
struct ClanHandlingResult {
  std::vector<protocol::ClanPacket> responses;
  bool close_connection = false;
};
struct ClanConnectionHandlers {
  std::function<ClanHandlingResult(const protocol::ClanPacket&)> on_packet;
  std::function<ClanHandlingResult()> on_tick;
};
using ClanHandlerFactory = std::function<ClanConnectionHandlers()>;

class ClanTcpServer {
 public:
  // Separate native framing and hello. Shares only socket/lifecycle machinery
  // with TcpServer; no main greeting, additive stream cipher or main packets.
  int Run(std::uint16_t port, const ClanHandlerFactory& create_handler,
          std::uint32_t idle_timeout_seconds = 30,
          const std::function<void(std::uint16_t)>& on_listening = {},
          std::stop_token stop = {},
          std::string_view listen_address = "127.0.0.1") const;
};
}  // namespace bubble_fighter::server::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_CLAN_TCP_SERVER_H_
