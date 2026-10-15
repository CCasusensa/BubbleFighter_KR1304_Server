#include "network/auth_tls_server.h"
#include "network/tcp_server.h"
#include "protocol/scenes/channel_transfer.h"
#include "sessions/connection_handoff.h"

#include <charconv>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace bf = bubble_fighter::server;
namespace {
void Require(bool value) {
  if (!value) throw std::runtime_error("Auth transport configuration test failed.");
}
}  // namespace

int RunAuthTransportTests() {
  using bf::network::TcpServer;
  Require(TcpServer::ResolveAdvertiseAddress("127.0.0.1") == 0x7f000001);
  const auto advertised =
      TcpServer::ResolveAdvertiseAddress("0.0.0.0", "203.0.113.27");
  Require(advertised == 0xcb00711b);
  Require(TcpServer::ResolveAdvertiseAddress("127.0.0.1", "203.0.113.27") ==
          advertised);
  for (const auto bad : {"0.0.0.0", "localhost", "127.1", "1.2.3.256",
                         "01.2.3.4", ""}) {
    bool rejected = false;
    try {
      TcpServer::ResolveAdvertiseAddress("0.0.0.0", bad);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Require(rejected);
  }
  const auto registry = std::make_shared<bf::sessions::ConnectionHandoffs>();
  registry->SetEndpoint(
      {static_cast<std::uint8_t>(advertised >> 24),
       static_cast<std::uint8_t>(advertised >> 16),
       static_cast<std::uint8_t>(advertised >> 8),
       static_cast<std::uint8_t>(advertised)}, 39001);
  bf::network::ConnectionHandlers handlers;
  handlers.on_packet = [](const auto&) {
    return bf::network::PacketHandlingResult{
        {bf::protocol::MoveChannelResponse{2, {}, 0, 60}.Encode()}, false};
  };
  auto connection = registry->Attach(
      std::move(handlers),
      {[] { return std::optional{bf::sessions::HandoffIdentity{7, 17}}; },
       [](std::uint32_t category) { return category == 60; }, [] {}});
  connection.handlers.on_packet(
      bf::protocol::HandoffCapabilityRequest{}.Encode());
  const auto offer = connection.handlers.on_packet(
      bf::protocol::MoveChannelRequest{60}.Encode());
  Require(offer.responses.size() == 2);
  const auto move = bf::protocol::MoveChannelResponse::Decode(offer.responses[1]);
  Require((move == bf::protocol::MoveChannelResponse{
                       0, {203, 0, 113, 27}, 39001, 60}));
  return 11;
}

int RunAuthTransportTestServer(std::string_view thumbprint,
                              std::string_view port_text) {
  unsigned int port = 0;
  const auto parsed = std::from_chars(
      port_text.data(), port_text.data() + port_text.size(), port);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != port_text.data() + port_text.size() || port == 0 ||
      port > 65535) {
    throw std::invalid_argument("Invalid test port.");
  }
  std::exception_ptr failure;
  std::promise<void> ready;
  auto future = ready.get_future();
  std::jthread worker([&](std::stop_token stop) {
    bool listening = false;
    try {
      bf::network::AuthTlsServer{}.Run(
          static_cast<std::uint16_t>(port), "127.0.0.1", thumbprint,
          bf::network::AuthTlsServer::RequestHandler{
              [](const bf::protocol::auth::AuthRequest& request,
                 std::string_view) {
                Require(request.action == bf::protocol::auth::Action::kHealth);
                return bf::protocol::auth::AuthResponse{};
              }},
          stop, [&](std::uint16_t) {
            listening = true;
            ready.set_value();
          });
    } catch (...) {
      failure = std::current_exception();
      if (!listening) ready.set_exception(failure);
    }
  });
  future.get();
  std::cin.get();
  worker.request_stop();
  worker.join();
  if (failure) std::rethrow_exception(failure);
  return 0;
}
