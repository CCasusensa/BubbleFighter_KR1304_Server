#ifndef BUBBLEFIGHTER_SERVER_OPERATIONS_OPERATIONS_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_OPERATIONS_OPERATIONS_SERVICE_H_

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>

#include "features/authentication/launcher_policy.h"
#include "network/tcp_server.h"
#include "operations/operation_sql.h"
#include "protocol/auth/auth_wire.h"

namespace bubble_fighter::server::operations {
class OperationsService {
 public:
  OperationsService(const std::filesystem::path& configuration,
                    const std::filesystem::path& database,
                    const std::filesystem::path& shop_directory);
  ~OperationsService();
  static OperationsService* Active() noexcept;
  std::uint16_t GmPort() const;
  void Start();
  void Stop();
  void RebuildShop();
  protocol::auth::AuthResponse Authenticate(
      const protocol::auth::AuthRequest& request, std::string_view peer,
      const features::authentication::LauncherPolicy& policy);
  bool Banned(std::uint64_t account, std::string_view peer);
  std::unique_lock<std::recursive_mutex> LockPlayers();
  std::shared_ptr<void> Connection();
  network::PacketHandlingResult PollConnection(
      const std::shared_ptr<void>& registration, std::uint64_t account,
      const Json& profile, network::PacketHandlingResult result);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace bubble_fighter::server::operations

#endif  // BUBBLEFIGHTER_SERVER_OPERATIONS_OPERATIONS_SERVICE_H_
