#ifndef BUBBLEFIGHTER_SERVER_NETWORK_TCP_SERVER_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_TCP_SERVER_H_

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "protocol/codec/tcp_packet_codec.h"

namespace bubble_fighter::server::network {
struct PacketHandlingResult {
  std::vector<protocol::TcpPacket> responses;
  bool close_connection = false;
};

using PacketHandler =
    std::function<PacketHandlingResult(const protocol::TcpPacket&)>;
struct ConnectionHandlers {
  PacketHandler on_packet;
  std::function<PacketHandlingResult()> on_tick;
};
using PacketHandlerFactory = std::function<ConnectionHandlers()>;

// Console log grouping. Every accepted connection gets its own tag so two
// concurrent sessions can be told apart in the shared stdout log; the tag is
// thread local because one connection is served by one thread.
void SetConnectionLogTag(std::string tag);
std::string_view ConnectionLogTag() noexcept;
std::string ConnectionLogPrefix();
std::string_view AcceptedPeerAddress() noexcept;

class TcpServer {
 public:
  // Strict dotted-decimal IPv4, returned in host byte order. This parses
  // an address only; Windows bind checks whether the interface exists.
  static std::uint32_t ParseListenAddress(std::string_view address);
  static std::uint32_t ResolveAdvertiseAddress(
      std::string_view listen_address, std::string_view advertise_address = {});
  // At most 64 concurrent clients; loopback is the default interface.
  // Port zero asks Windows to select an available port.
  int Run(std::uint16_t port, bool once,
          const PacketHandlerFactory& create_handler,
          std::uint32_t idle_timeout_seconds = 30,
          const std::function<void(std::uint16_t)>& on_listening = {},
          std::stop_token stop = {},
          std::string_view listen_address = "127.0.0.1") const;
};
}  // namespace bubble_fighter::server::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_TCP_SERVER_H_
