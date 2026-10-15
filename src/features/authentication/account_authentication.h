#ifndef BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_ACCOUNT_AUTHENTICATION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_ACCOUNT_AUTHENTICATION_H_

#include "features/authentication/account_store.h"

namespace bubble_fighter::server::features::authentication {
class AccountAuthentication {
 public:
  static constexpr std::uint32_t kTicketLifetimeSeconds = 300;

  explicit AccountAuthentication(AccountStore& store);
  // Local administrative operation. This is not a public registration API.
  bool CreateAccount(std::string_view username, std::string_view password);
  std::optional<std::string> Login(std::string_view username,
                                   std::string_view password);
  std::optional<std::uint64_t> ConsumeTicket(std::string_view ticket);
  // Administrative lookup used by registration-time provisioning; it grants
  // no authentication by itself.
  std::optional<std::uint64_t> FindAccountId(std::string_view username);
  static std::string NormalizeUsername(std::string_view username);

 private:
  AccountStore& store_;
};
}  // namespace bubble_fighter::server::features::authentication

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_ACCOUNT_AUTHENTICATION_H_
