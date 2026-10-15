#include "features/authentication/remote_auth.h"

#include <algorithm>

namespace bubble_fighter::server::features::authentication {
namespace {
using Action = protocol::auth::Action;
using Response = protocol::auth::AuthResponse;
using Error = protocol::auth::ErrorCode;
template <typename State>
void Prune(State& state,
           std::chrono::steady_clock::time_point now) {
  const auto window = std::chrono::minutes(5);
  while (!state.failures.empty() && now - state.failures.front() >= window)
    state.failures.pop_front();
  while (!state.registrations.empty() &&
         now - state.registrations.front() >= window)
    state.registrations.pop_front();
  if (state.suspended_until != std::chrono::steady_clock::time_point{} &&
      state.suspended_until <= now) {
    state.suspended_until = {};
    state.failures.clear();
  }
}
Response Failure(Error error) {
  Response response;
  response.status = protocol::auth::Status::kError;
  response.error = error;
  response.message = error == Error::kDuplicate
                         ? "Account already exists."
                         : error == Error::kRateLimited
                               ? "Too many attempts. Try again later."
                               : error == Error::kBadRequest
                                     ? "Invalid account request."
                                     : "Authentication failed.";
  return response;
}
}  // namespace

bool AttemptLimiter::Allow(std::string_view peer_ip, Action action,
                           std::chrono::steady_clock::time_point now) {
  std::lock_guard lock(mutex_);
  for (auto& [ip, state] : states_) Prune(state, now);
  std::erase_if(states_, [now](const auto& entry) {
    return entry.second.failures.empty() && entry.second.registrations.empty() &&
           entry.second.suspended_until <= now &&
           now - entry.second.window_start >= std::chrono::minutes(5);
  });
  auto iterator = states_.find(std::string(peer_ip));
  if (iterator == states_.end()) {
    if (states_.size() >= 4096) return false;
    iterator = states_.emplace(std::string(peer_ip), State{now}).first;
  }
  auto& state = iterator->second;
  Prune(state, now);
  if (action == Action::kLogin) {
    if (state.suspended_until > now) return false;
  } else if (action == Action::kRegister && state.registrations.size() >= 5) {
    return false;
  }
  if (action == Action::kRegister) state.registrations.push_back(now);
  return true;
}
void AttemptLimiter::RecordLoginFailure(std::string_view peer_ip,
                                        std::chrono::steady_clock::time_point now) {
  std::lock_guard lock(mutex_);
  auto iterator = states_.find(std::string(peer_ip));
  if (iterator == states_.end()) return;
  auto& state = iterator->second;
  Prune(state, now);
  state.failures.push_back(now);
  if (state.failures.size() >= 10) {
    state.suspended_until = now + std::chrono::seconds(60);
  }
}
void AttemptLimiter::RecordLoginSuccess(std::string_view peer_ip) {
  std::lock_guard lock(mutex_);
  const auto iterator = states_.find(std::string(peer_ip));
  if (iterator != states_.end()) {
    iterator->second.failures.clear();
    iterator->second.suspended_until = {};
  }
}

Response RemoteAuth::Handle(const protocol::auth::AuthRequest& request,
                            AccountAuthentication& accounts,
                            std::string_view peer_ip,
                            std::chrono::steady_clock::time_point now) {
  if (request.action == Action::kHealth) return {};
  if (request.action != Action::kRegister && request.action != Action::kLogin)
    return Failure(Error::kUnsupported);
  if (!limiter_.Allow(peer_ip, request.action, now))
    return Failure(Error::kRateLimited);
  try {
    if (request.action == Action::kRegister) {
      if (!accounts.CreateAccount(request.username, request.password))
        return Failure(Error::kDuplicate);
      return {};
    }
    const auto ticket = accounts.Login(request.username, request.password);
    if (!ticket) {
      limiter_.RecordLoginFailure(peer_ip, now);
      return Failure(Error::kInvalidCredentials);
    }
    limiter_.RecordLoginSuccess(peer_ip);
    Response response;
    response.username = AccountAuthentication::NormalizeUsername(request.username);
    response.ticket = *ticket;
    response.expires_at =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count()) +
        AccountAuthentication::kTicketLifetimeSeconds;
    return response;
  } catch (const std::invalid_argument&) {
    return Failure(Error::kBadRequest);
  } catch (...) {
    return Failure(Error::kUnavailable);
  }
}
}  // namespace bubble_fighter::server::features::authentication
