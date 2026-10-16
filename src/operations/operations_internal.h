#ifndef BUBBLEFIGHTER_SERVER_OPERATIONS_OPERATIONS_INTERNAL_H_
#define BUBBLEFIGHTER_SERVER_OPERATIONS_OPERATIONS_INTERNAL_H_

#include <httplib.h>
#include <atomic>
#include <thread>
#include "operations/operations_service.h"
#include "features/authentication/remote_auth.h"
#include "persistence/my_sql_connection.h"

namespace bubble_fighter::server::operations {
struct OnlineConnection {
  std::uint64_t account = 0;
  Json profile;
  bool kick = false;
  std::vector<protocol::TcpPacket> messages;
};
struct OperationsService::Impl {
  Json config;
  std::filesystem::path database, shop_directory;
  httplib::Server gm;
  std::jthread gm_thread;
  std::recursive_mutex gate;
  features::authentication::AttemptLimiter limiter;
  std::vector<std::weak_ptr<OnlineConnection>> online;
  Impl(const std::filesystem::path& configuration,
       const std::filesystem::path& database_path,
       const std::filesystem::path& shop);
  persistence::MySqlConnection Db() const { return persistence::MySqlConnection(database); }
  Json Settings(MYSQL* db);
  bool Banned(MYSQL* db, std::uint64_t account, std::string_view peer);
  Json Verify(MYSQL* db, const Json& input, std::string_view peer);
  Json Register(MYSQL* db, const Json& input, bool administrative = false);
  Json Profile(MYSQL* db, std::uint64_t account, bool include_items = true);
  std::string NewSession(MYSQL* db, Json account, std::string_view purpose);
  Json Session(MYSQL* db, std::string_view token, std::string_view purpose);
  Json Authenticate(MYSQL* db, protocol::auth::Action action, const Json& input,
                    std::string_view peer);
  void Routes();
  Json Api(MYSQL* db, const httplib::Request& request, httplib::Response& response,
           const Json& input, std::string_view actor);
  std::string Authorize(MYSQL* db, const httplib::Request& request);
  bool AllowIp(MYSQL* db, std::string_view peer);
  void Kick(std::uint64_t account);
  void Audit(MYSQL* db, std::string_view actor, const httplib::Request& request,
             const Json& input, const Json& before, const Json& after, bool ok);
  void Grant(MYSQL* db, std::uint64_t account, const Json& items);
  void Bag(MYSQL* db, std::uint64_t account, std::uint32_t item, const Json& change);
  void Stats(MYSQL* db, std::uint64_t account, const Json& input);
  Json Catalog(MYSQL* db);
  Json Rebuild(MYSQL* db);
};
std::string Base64(std::string_view bytes);
std::string Unbase64(std::string_view bytes);
std::string ReadText(const std::filesystem::path& path, std::size_t limit = 8 * 1024 * 1024);
std::uint32_t Bounded(const Json& input, std::string_view key,
                      std::uint32_t fallback = 0, std::uint32_t minimum = 0);
Json Statistics(std::string_view bytes);
const Json& CatalogCategories();
Json CharacterOptions();
std::string CharacterName(std::uint32_t type);
std::string ItemName(std::uint32_t item);
void DescribeCategory(Json& row);
}  // namespace bubble_fighter::server::operations
#endif  // BUBBLEFIGHTER_SERVER_OPERATIONS_OPERATIONS_INTERNAL_H_
