#ifndef BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_REMOTE_AUTH_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_REMOTE_AUTH_H_

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "features/authentication/account_authentication.h"
#include "features/authentication/launcher_policy.h"
#include "protocol/auth/auth_wire.h"

namespace bubble_fighter::server::features::authentication {
class AttemptLimiter {
 public:
  bool Allow(std::string_view peer_ip, protocol::auth::Action action,
             std::chrono::steady_clock::time_point now);
  void RecordLoginFailure(std::string_view peer_ip,
                          std::chrono::steady_clock::time_point now);
  void RecordLoginSuccess(std::string_view peer_ip);

 private:
  struct State {
    std::chrono::steady_clock::time_point window_start{};
    std::chrono::steady_clock::time_point suspended_until{};
    std::deque<std::chrono::steady_clock::time_point> failures;
    std::deque<std::chrono::steady_clock::time_point> registrations;
  };
  std::mutex mutex_;
  std::unordered_map<std::string, State> states_;
};

class RemoteAuth {
 public:
  explicit RemoteAuth(AttemptLimiter& limiter, LauncherPolicy policy = {})
      : limiter_(limiter), policy_(std::move(policy)) {}
  protocol::auth::AuthResponse Handle(
      const protocol::auth::AuthRequest& request, AccountAuthentication& accounts,
      std::string_view peer_ip, std::chrono::steady_clock::time_point now);

 private:
  AttemptLimiter& limiter_;
  LauncherPolicy policy_;
};
}  // namespace bubble_fighter::server::features::authentication

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_REMOTE_AUTH_H_
