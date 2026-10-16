#include "protocol/auth/auth_wire.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <stdexcept>
#include <string_view>
#include <json.hpp>
#include "protocol/auth/auth_json.h"

namespace bubble_fighter::server::protocol::auth {
namespace {
[[noreturn]] void Invalid() {
  throw std::invalid_argument("Invalid authentication frame.");
}
void ValidateText(std::string_view text) {
  if (text.size() > kMaxPayloadBytes ||
      text.find('\0') != std::string_view::npos ||
      (!text.empty() &&
       !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), nullptr, 0))) Invalid();
}
void Write(std::vector<std::uint8_t>& bytes, std::uint64_t value, int width) {
  for (int shift = (width - 1) * 8; shift >= 0; shift -= 8)
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void Text(std::vector<std::uint8_t>& bytes, std::string_view text) {
  ValidateText(text);
  Write(bytes, text.size(), 2);
  bytes.insert(bytes.end(), text.begin(), text.end());
}
std::vector<std::uint8_t> Frame(std::vector<std::uint8_t> payload) {
  if (payload.size() > (payload.front() == 2 ? kMaxJsonPayloadBytes : kMaxPayloadBytes)) Invalid();
  std::vector<std::uint8_t> bytes;
  Write(bytes, payload.size(), 4);
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}
class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> frame) : bytes_(frame) {
    const auto size = Read(4);
    if (size < 2 || size > kMaxPayloadBytes || size != bytes_.size()) Invalid();
    if (Read(1) != 1) Invalid();
  }
  std::uint64_t Read(std::size_t width) {
    if (bytes_.size() < width) Invalid();
    std::uint64_t value = 0;
    for (auto byte : bytes_.first(width)) value = (value << 8) | byte;
    bytes_ = bytes_.subspan(width);
    return value;
  }
  std::string Text() {
    const auto size = static_cast<std::size_t>(Read(2));
    if (bytes_.size() < size) Invalid();
    std::string text(reinterpret_cast<const char*>(bytes_.data()), size);
    ValidateText(text);
    bytes_ = bytes_.subspan(size);
    return text;
  }
  bool Empty() const { return bytes_.empty(); }
  void Finish() const { if (!Empty()) Invalid(); }
 private:
  std::span<const std::uint8_t> bytes_;
};
bool ValidAction(Action action) {
  return action == Action::kHealth || action == Action::kRegister ||
         action == Action::kLogin || action == Action::kPolicy;
}
bool carries_credentials(Action action) {
  return action == Action::kRegister || action == Action::kLogin;
}
bool ValidError(ErrorCode error) {
  return error >= ErrorCode::kBadRequest && error <= ErrorCode::kUnavailable;
}
}  // namespace

std::vector<std::uint8_t> EncodeRequest(const AuthRequest& request) {
  if (request.version == 2) {
    if (request.action < Action::kHealth || request.action > Action::kTicket ||
        !ParseBoundedJson(request.json).is_object()) Invalid();
    std::vector<std::uint8_t> bytes{2, static_cast<std::uint8_t>(request.action)};
    bytes.insert(bytes.end(), request.json.begin(), request.json.end());
    return Frame(std::move(bytes));
  }
  if (request.version != 1) Invalid();
  if (!ValidAction(request.action)) Invalid();
  std::vector<std::uint8_t> bytes{1, static_cast<std::uint8_t>(request.action)};
  if (carries_credentials(request.action)) {
    Text(bytes, request.username);
    Text(bytes, request.password);
    if (!request.character_name.empty()) {
      if (request.action != Action::kRegister) Invalid();
      Text(bytes, request.character_name);
    }
  } else if (!request.username.empty() || !request.password.empty()) Invalid();
  if (!carries_credentials(request.action) && !request.character_name.empty())
    Invalid();
  return Frame(std::move(bytes));
}
AuthRequest DecodeRequest(std::span<const std::uint8_t> frame) {
  if (frame.size() >= 6 && frame[4] == 2) {
    std::uint32_t size = 0;
    for (auto byte : frame.first(4)) size = (size << 8) | byte;
    if (size < 4 || size > kMaxJsonPayloadBytes || size != frame.size() - 4 ||
        frame[5] < 1 || frame[5] > 11) Invalid();
    AuthRequest request;
    request.version = 2;
    request.action = static_cast<Action>(frame[5]);
    request.json.assign(reinterpret_cast<const char*>(frame.data() + 6), frame.size() - 6);
    try {
      (void)ParseBoundedJson(request.json);
    } catch (const nlohmann::json::exception&) { Invalid(); }
    return request;
  }
  Reader reader(frame);
  AuthRequest request;
  request.action = static_cast<Action>(reader.Read(1));
  if (!ValidAction(request.action)) Invalid();
  if (carries_credentials(request.action)) {
    request.username = reader.Text();
    request.password = reader.Text();
    if (request.action == Action::kRegister && !reader.Empty())
      request.character_name = reader.Text();
  }
  reader.Finish();
  return request;
}
std::vector<std::uint8_t> EncodeResponse(const AuthResponse& response) {
  if (response.version == 2) {
    (void)ParseBoundedJson(response.json);
    std::vector<std::uint8_t> bytes{2, static_cast<std::uint8_t>(response.status)};
    bytes.insert(bytes.end(), response.json.begin(), response.json.end());
    return Frame(std::move(bytes));
  }
  std::vector<std::uint8_t> bytes{1, static_cast<std::uint8_t>(response.status)};
  if (response.status == Status::kError) {
    if (!ValidError(response.error) || response.message.size() > 256 ||
        !response.username.empty() || !response.ticket.empty() ||
        response.expires_at) Invalid();
    Write(bytes, static_cast<std::uint16_t>(response.error), 2);
    Text(bytes, response.message);
  } else if (response.status == Status::kOk) {
    if (!response.message.empty()) Invalid();
    if (response.policy) {
      // Policy answers always carry both tables, empty ones included.
      if (!response.username.empty() || !response.ticket.empty() ||
          response.expires_at ||
          response.modes.size() > kMaxPolicyModes ||
          response.crazy_modes.size() > kMaxPolicyModes) Invalid();
      Write(bytes, response.modes.size(), 2);
      for (auto mode : response.modes) Write(bytes, mode, 2);
      Write(bytes, response.crazy_modes.size(), 2);
      for (auto mode : response.crazy_modes) Write(bytes, mode, 2);
    } else if (!response.username.empty() || !response.ticket.empty() || response.expires_at) {
      if (!response.modes.empty() || !response.crazy_modes.empty()) Invalid();
      if (response.username.empty() || response.ticket.empty() || !response.expires_at) Invalid();
      Text(bytes, response.username);
      Text(bytes, response.ticket);
      Write(bytes, response.expires_at, 8);
    } else if (!response.modes.empty() || !response.crazy_modes.empty()) Invalid();
  } else Invalid();
  return Frame(std::move(bytes));
}
AuthResponse DecodePolicyResponse(std::span<const std::uint8_t> frame) {
  Reader reader(frame);
  AuthResponse response;
  response.status = static_cast<Status>(reader.Read(1));
  if (response.status == Status::kError) {
    response.error = static_cast<ErrorCode>(reader.Read(2));
    response.message = reader.Text();
    if (!ValidError(response.error) || response.message.size() > 256) Invalid();
    reader.Finish();
    return response;
  }
  if (response.status != Status::kOk) Invalid();
  response.policy = true;
  const auto modes = static_cast<std::size_t>(reader.Read(2));
  if (modes > kMaxPolicyModes) Invalid();
  for (std::size_t index = 0; index < modes; ++index)
    response.modes.push_back(static_cast<std::uint16_t>(reader.Read(2)));
  const auto crazy = static_cast<std::size_t>(reader.Read(2));
  if (crazy > kMaxPolicyModes) Invalid();
  for (std::size_t index = 0; index < crazy; ++index)
    response.crazy_modes.push_back(static_cast<std::uint16_t>(reader.Read(2)));
  reader.Finish();
  return response;
}
AuthResponse DecodeResponse(std::span<const std::uint8_t> frame) {
  if (frame.size() >= 6 && frame[4] == 2) {
    std::uint32_t size = 0;
    for (auto byte : frame.first(4)) size = (size << 8) | byte;
    if (size < 4 || size > kMaxJsonPayloadBytes || size != frame.size() - 4 ||
        frame[5] > 1) Invalid();
    AuthResponse response;
    response.version = 2;
    response.status = static_cast<Status>(frame[5]);
    response.json.assign(reinterpret_cast<const char*>(frame.data() + 6), frame.size() - 6);
    try {
      (void)ParseBoundedJson(response.json);
    } catch (const nlohmann::json::exception&) { Invalid(); }
    return response;
  }
  Reader reader(frame);
  AuthResponse response;
  response.status = static_cast<Status>(reader.Read(1));
  if (response.status == Status::kError) {
    response.error = static_cast<ErrorCode>(reader.Read(2));
    response.message = reader.Text();
    if (!ValidError(response.error) || response.message.size() > 256) Invalid();
  } else if (response.status == Status::kOk) {
    if (!reader.Empty()) {
      response.username = reader.Text();
      response.ticket = reader.Text();
      response.expires_at = reader.Read(8);
      if (response.username.empty() || response.ticket.empty() || !response.expires_at) Invalid();
    }
  } else Invalid();
  reader.Finish();
  return response;
}
}  // namespace bubble_fighter::server::protocol::auth
