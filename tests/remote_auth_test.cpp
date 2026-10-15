#include "features/authentication/remote_auth.h"

#include <chrono>
#include <stdexcept>

namespace {
namespace auth = bubble_fighter::server::features::authentication;
namespace wire = bubble_fighter::server::protocol::auth;
struct Store final : auth::AccountStore {
  std::optional<auth::StoredAccount> account;
  std::string ticket;
  int tickets_added = 0;
  bool CreateAccount(std::string_view name,
                     const auth::PasswordVerifier& password) override {
    if (account) return false;
    account = auth::StoredAccount{7, password, false};
    return name == "alice42";
  }
  std::optional<auth::StoredAccount> FindAccount(
      std::string_view name) override {
    return name == "alice42" ? account : std::nullopt;
  }
  bool AddTicket(std::uint64_t id, const auth::CredentialDigest&,
                 std::uint32_t) override {
    ++tickets_added;
    if (id != 7) return false;
    ticket = "issued";
    return true;
  }
  std::optional<std::uint64_t> ConsumeTicket(
      const auth::CredentialDigest&) override {
    return std::nullopt;
  }
};
void Require(bool value) {
  if (!value) throw std::runtime_error("Remote auth test failed.");
}
}  // namespace

int RunRemoteAuthTests() {
  Store store;
  auth::AccountAuthentication accounts(store);
  auth::AttemptLimiter limiter;
  auth::RemoteAuth remote(limiter);
  const auto now = std::chrono::steady_clock::now();
  auto health = remote.Handle({wire::Action::kHealth}, accounts, "10.0.0.1", now);
  Require(health.status == wire::Status::kOk);
  auto short_name = remote.Handle(
      {wire::Action::kRegister, "Alice", "test-password-123"}, accounts,
      "10.0.0.7", now);
  Require(short_name.status == wire::Status::kError &&
          short_name.error == wire::ErrorCode::kBadRequest);
  auto short_password = remote.Handle(
      {wire::Action::kRegister, "Alice42", "12345"}, accounts, "10.0.0.8", now);
  Require(short_password.status == wire::Status::kError &&
          short_password.error == wire::ErrorCode::kBadRequest);
  auto created = remote.Handle(
      {wire::Action::kRegister, "Alice42", "test-password-123"}, accounts,
      "10.0.0.1", now);
  Require(created.status == wire::Status::kOk);
  auto duplicate = remote.Handle(
      {wire::Action::kRegister, "Alice42", "test-password-123"}, accounts,
      "10.0.0.1", now);
  Require(duplicate.error == wire::ErrorCode::kDuplicate);
  const auto before_login = std::chrono::system_clock::now();
  auto login = remote.Handle(
      {wire::Action::kLogin, "ALICE42", "test-password-123"}, accounts,
      "10.0.0.1", now);
  const auto after_login = std::chrono::system_clock::now();
  const auto seconds = [](auto time) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            time.time_since_epoch()).count());
  };
  Require(login.status == wire::Status::kOk && login.ticket.size() == 69 &&
          login.ticket.starts_with("BFS1.") &&
          login.expires_at >= seconds(before_login) + 300 &&
          login.expires_at <= seconds(after_login) + 300);
  const auto tickets_before_failure = store.tickets_added;
  const auto unknown = remote.Handle(
      {wire::Action::kLogin, "unknown", "wrong-password"}, accounts,
      "10.0.0.9", now);
  const auto wrong_password = remote.Handle(
      {wire::Action::kLogin, "alice42", "wrong-password"}, accounts,
      "10.0.0.9", now);
  Require(unknown.status == wire::Status::kError &&
          unknown.error == wire::ErrorCode::kInvalidCredentials &&
          wrong_password.status == unknown.status &&
          wrong_password.error == unknown.error &&
          wrong_password.message == unknown.message &&
          unknown.ticket.empty() && wrong_password.ticket.empty() &&
          store.tickets_added == tickets_before_failure);
  for (int i = 0; i < 10; ++i) {
    auto bad = remote.Handle(
        {wire::Action::kLogin, "alice42", "wrong-password"}, accounts,
        "10.0.0.2", now + std::chrono::seconds(i));
    Require(bad.error == wire::ErrorCode::kInvalidCredentials);
  }
  auto limited = remote.Handle(
      {wire::Action::kLogin, "alice42", "wrong-password"}, accounts, "10.0.0.2",
      now + std::chrono::seconds(10));
  Require(limited.error == wire::ErrorCode::kRateLimited);
  Require(limiter.Allow("10.0.0.2", wire::Action::kLogin,
                        now + std::chrono::seconds(69)));
  Require(limiter.Allow("10.0.0.3", wire::Action::kLogin, now));
  for (int i = 0; i < 5; ++i)
    Require(limiter.Allow("10.0.0.4", wire::Action::kRegister, now));
  limiter.RecordLoginSuccess("10.0.0.4");
  Require(!limiter.Allow("10.0.0.4", wire::Action::kRegister, now));
  Require(limiter.Allow("10.0.0.4", wire::Action::kRegister,
                        now + std::chrono::minutes(5)));
  auth::AttemptLimiter bounded;
  for (int i = 0; i < 4096; ++i)
    Require(bounded.Allow(std::to_string(i), wire::Action::kRegister, now));
  Require(!bounded.Allow("overflow", wire::Action::kLogin, now));
  Require(bounded.Allow("overflow", wire::Action::kLogin,
                        now + std::chrono::minutes(5)));
  return 7;
}
