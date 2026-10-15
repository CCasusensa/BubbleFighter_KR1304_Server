#ifndef BUBBLEFIGHTER_SERVER_NETWORK_AUTH_TLS_SERVER_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_AUTH_TLS_SERVER_H_

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string_view>

#include "protocol/auth/auth_wire.h"
#include "features/authentication/launcher_policy.h"

namespace bubble_fighter::server::network {
class AuthTlsServer {
 public:
  using RequestHandler = std::function<protocol::auth::AuthResponse(
      const protocol::auth::AuthRequest&, std::string_view)>;

  static void ValidateConfiguration(
      std::string_view certificate_thumbprint,
      const std::filesystem::path& database_configuration);

  int Run(std::uint16_t port, std::string_view listen_address,
          std::string_view certificate_thumbprint,
          const std::filesystem::path& database_configuration,
          std::stop_token stop = {},
          const std::function<void(std::uint16_t)>& on_listening = {}) const;

  // Same listener with an operator-configured launcher policy: kPolicy requests
  // answer with the build-a-room tables read from the policy file.
  int Run(std::uint16_t port, std::string_view listen_address,
          std::string_view certificate_thumbprint,
          const std::filesystem::path& database_configuration,
          features::authentication::LauncherPolicy policy,
          std::stop_token stop = {},
          const std::function<void(std::uint16_t)>& on_listening = {}) const;

  // The transport owns TLS, framing and admission; the callback owns accounts.
  int Run(std::uint16_t port, std::string_view listen_address,
          std::string_view certificate_thumbprint,
          const RequestHandler& handler, std::stop_token stop = {},
          const std::function<void(std::uint16_t)>& on_listening = {}) const;
};
}  // namespace bubble_fighter::server::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_AUTH_TLS_SERVER_H_
