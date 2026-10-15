#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_AUTH_AUTH_WIRE_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_AUTH_AUTH_WIRE_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bubble_fighter::server::protocol::auth {
inline constexpr std::size_t kMaxPayloadBytes = 4096;
// kPolicy asks the server for the launcher's build-a-room mode tables.
enum class Action : std::uint8_t {
  kHealth = 1, kRegister = 2, kLogin = 3, kPolicy = 4
};
enum class Status : std::uint8_t { kOk = 0, kError = 1 };
enum class ErrorCode : std::uint16_t {
  kBadRequest = 1, kUnsupported = 2, kDuplicate = 3,
  kInvalidCredentials = 4, kRateLimited = 5, kUnavailable = 6
};
struct AuthRequest {
  Action action = Action::kHealth;
  std::string username;
  std::string password;
  // kRegister only, optional: the character name the account should start
  // with. Older launchers leave it empty and keep working; the server then
  // answers the registration without a character.
  std::string character_name;
};
struct AuthResponse {
  Status status = Status::kOk;
  ErrorCode error = ErrorCode::kBadRequest;
  std::string message;
  std::string username;
  std::string ticket;
  std::uint64_t expires_at = 0;
  // kPolicy response: the launcher policy tables, in the announced order.
  bool policy = false;
  std::vector<std::uint16_t> modes;
  std::vector<std::uint16_t> crazy_modes;
};
inline constexpr std::size_t kMaxPolicyModes = 256;
std::vector<std::uint8_t> EncodeRequest(const AuthRequest& request);
AuthRequest DecodeRequest(std::span<const std::uint8_t> frame);
std::vector<std::uint8_t> EncodeResponse(const AuthResponse& response);
AuthResponse DecodeResponse(std::span<const std::uint8_t> frame);
AuthResponse DecodePolicyResponse(std::span<const std::uint8_t> frame);
}  // namespace bubble_fighter::server::protocol::auth

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_AUTH_AUTH_WIRE_H_
