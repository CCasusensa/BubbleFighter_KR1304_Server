#include "protocol/auth/auth_wire.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace wire = bubble_fighter::server::protocol::auth;
void Require(bool condition) {
  if (!condition) throw std::runtime_error("Remote auth wire contract failed.");
}
template <typename Operation>
void Reject(Operation operation) {
  try {
    operation();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("Invalid remote auth frame accepted.");
}
}  // namespace

int RunAuthWireTests() {
  const auto frame =
      wire::EncodeRequest({wire::Action::kLogin, "Alice", "test-password-123"});
  const std::vector<std::uint8_t> expected{
      0, 0, 0, 28, 1, 3, 0, 5, 'A', 'l', 'i', 'c', 'e', 0, 17,
      't', 'e', 's', 't', '-', 'p', 'a', 's', 's', 'w', 'o', 'r', 'd', '-',
      '1', '2', '3'};
  Require(frame == expected);
  const auto request = wire::DecodeRequest(frame);
  Require(request.action == wire::Action::kLogin && request.username == "Alice" &&
          request.password == "test-password-123");
  for (std::size_t size = 0; size < frame.size(); ++size) {
    Reject([&] { wire::DecodeRequest(std::span(frame).first(size)); });
  }
  auto extra = frame;
  extra.push_back(0);
  Reject([&] { wire::DecodeRequest(extra); });
  ++extra[3];
  Reject([&] { wire::DecodeRequest(extra); });
  auto bad = frame;
  bad[4] = 2;
  Reject([&] { wire::DecodeRequest(bad); });
  bad = frame;
  bad[5] = 4;
  Reject([&] { wire::DecodeRequest(bad); });
  bad = frame;
  bad[8] = 0xff;
  Reject([&] { wire::DecodeRequest(bad); });
  bad[8] = 0;
  Reject([&] { wire::DecodeRequest(bad); });
  Reject([] {
    std::vector<std::uint8_t> oversized(4101);
    oversized[2] = 0x10;
    oversized[3] = 0x01;  // Big-endian payload length 4097.
    wire::DecodeRequest(oversized);
  });
  Reject([] { wire::EncodeRequest({wire::Action::kLogin, "abc", std::string(4096, 'x')}); });

  wire::AuthResponse login;
  login.username = "alice";
  login.ticket = "BFS1." + std::string(64, 'a');
  login.expires_at = 0x0102030405060708ULL;
  const auto encoded = wire::EncodeResponse(login);
  const auto decoded = wire::DecodeResponse(encoded);
  Require(decoded.username == login.username && decoded.ticket == login.ticket &&
          decoded.expires_at == login.expires_at);
  Require(encoded.back() == 8 && encoded[encoded.size() - 8] == 1);
  wire::AuthResponse error;
  error.status = wire::Status::kError;
  error.error = wire::ErrorCode::kInvalidCredentials;
  error.message = "Authentication failed";
  const auto rejected = wire::DecodeResponse(wire::EncodeResponse(error));
  Require(rejected.error == error.error && rejected.message == error.message &&
          rejected.ticket.empty());
  Require(wire::DecodeResponse(wire::EncodeResponse({})).ticket.empty());
  return static_cast<int>(frame.size()) + 15;
}
