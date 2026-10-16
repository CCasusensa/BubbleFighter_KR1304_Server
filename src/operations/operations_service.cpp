#include "operations/operations_internal.h"

#include <wincrypt.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include "features/authentication/credential_crypto.h"
#include "features/chat/scene_chat.h"

namespace bubble_fighter::server::operations {
namespace {
std::atomic<OperationsService*> active{};
}
std::string ReadText(const std::filesystem::path& path, std::size_t limit) {
  if (std::filesystem::file_size(path) > limit) throw std::invalid_argument("File exceeds limit.");
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot read file.");
  return {std::istreambuf_iterator<char>(stream), {}};
}
std::string Base64(std::string_view bytes) {
  if (bytes.empty()) return {};
  DWORD length = 0;
  const auto* data = reinterpret_cast<const BYTE*>(bytes.data());
  if (!CryptBinaryToStringA(data, static_cast<DWORD>(bytes.size()),
       CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length))
    throw std::runtime_error("Base64 encoding failed.");
  std::string result(length, '\0');
  if (!CryptBinaryToStringA(data, static_cast<DWORD>(bytes.size()),
       CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &length))
    throw std::runtime_error("Base64 encoding failed.");
  result.resize(length);
  return result;
}
std::string Unbase64(std::string_view text) {
  DWORD length = 0;
  if (text.empty()) return {};
  if (text.size() > 87384 || text.size() % 4) throw std::invalid_argument("Invalid cloud bytes.");
  if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()),
       CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &length, nullptr, nullptr) ||
       length > 65536) throw std::invalid_argument("Invalid cloud bytes.");
  std::string result(length, '\0');
  if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()),
       CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
       reinterpret_cast<BYTE*>(result.data()), &length, nullptr, nullptr))
    throw std::invalid_argument("Invalid cloud bytes.");
  result.resize(length);
  if (Base64(result) != text) throw std::invalid_argument("Noncanonical cloud bytes.");
  return result;
}
std::uint32_t Bounded(const Json& input, std::string_view key,
                      std::uint32_t fallback, std::uint32_t minimum) {
  const auto found = input.find(std::string(key));
  if (found == input.end()) return fallback;
  if (!found->is_number_integer()) throw std::invalid_argument("Expected integer.");
  const auto value = found->get<std::int64_t>();
  if (value < minimum || value > 0x7fffffff) throw std::invalid_argument("Value outside KR limits.");
  return static_cast<std::uint32_t>(value);
}
OperationsService::Impl::Impl(const std::filesystem::path& configuration,
                             const std::filesystem::path& database_path,
                             const std::filesystem::path& shop)
    : config(ReadJson(configuration)), database(database_path), shop_directory(shop) {
  const auto resolve = [&](const char* key) {
    if (config.contains(key)) config[key] = std::filesystem::absolute(
        configuration.parent_path() / config.at(key).get<std::string>()).string();
  };
  resolve("web_root");
  for (const auto* key : {"bootstrap_token", "bot_token"}) {
    const auto token = config.value(key, std::string{});
    if (!token.empty() && token.size() < 32) throw std::invalid_argument("Operations tokens need at least 32 characters.");
  }
  if (config.value("bootstrap_token", "") == config.value("bot_token", "") &&
      !config.value("bot_token", "").empty())
    throw std::invalid_argument("Bootstrap and bot tokens must differ.");
  auto db = Db();
  Settings(db.Handle());
  Routes();
}
OperationsService::OperationsService(const std::filesystem::path& configuration,
                                    const std::filesystem::path& database,
                                    const std::filesystem::path& shop)
    : impl_(std::make_unique<Impl>(configuration, database, shop)) {
  OperationsService* expected = nullptr;
  if (!active.compare_exchange_strong(expected, this))
    throw std::logic_error("Operations already running.");
}
OperationsService::~OperationsService() { Stop(); active.store(nullptr); }
OperationsService* OperationsService::Active() noexcept { return active.load(); }
std::uint16_t OperationsService::GmPort() const {
  const auto value = impl_->config.value("gm_port", 21582);
  if (value < 1 || value > 65535) throw std::invalid_argument("Invalid GM port.");
  return static_cast<std::uint16_t>(value);
}
void OperationsService::Start() {
  const auto gm_host = impl_->config.value("gm_address", std::string("127.0.0.1"));
  network::TcpServer::ParseListenAddress(gm_host);
  if (!impl_->gm.bind_to_port(gm_host, GmPort())) {
    Stop();
    throw std::runtime_error("Cannot bind GM port.");
  }
  impl_->gm_thread = std::jthread([this] { impl_->gm.listen_after_bind(); });
}
void OperationsService::Stop() {
  impl_->gm.stop();
  if (impl_->gm_thread.joinable()) impl_->gm_thread.join();
}
void OperationsService::RebuildShop() {
  auto lock = LockPlayers();
  auto db = impl_->Db();
  (void)impl_->Rebuild(db.Handle());
}
std::unique_lock<std::recursive_mutex> OperationsService::LockPlayers() {
  return std::unique_lock(impl_->gate);
}
bool OperationsService::Banned(std::uint64_t account, std::string_view peer) {
  auto db = impl_->Db();
  return impl_->Banned(db.Handle(), account, peer);
}
bool OperationsService::Impl::Banned(MYSQL* db, std::uint64_t account, std::string_view peer) {
  return !Query(db, "SELECT BanId FROM OperationBans WHERE "
      "((Kind='account' AND Target=?) OR (Kind='ip' AND Target=?)) "
      "AND (ExpiresAt=0 OR ExpiresAt>?) LIMIT 1",
      {std::to_string(account), std::string(peer), Now()}).empty();
}
Json OperationsService::Impl::Settings(MYSQL* db) {
  const auto rows = Query(db, "SELECT Document FROM OperationSettings WHERE SettingId=1");
  if (rows.size() != 1) throw std::runtime_error("Operations migration is missing.");
  return Json::parse(rows[0]["Document"].get<std::string>());
}
std::shared_ptr<void> OperationsService::Connection() {
  auto lock = LockPlayers();
  std::erase_if(impl_->online, [](const auto& item) { return item.expired(); });
  auto registration = std::make_shared<OnlineConnection>();
  impl_->online.push_back(registration);
  return registration;
}
network::PacketHandlingResult OperationsService::PollConnection(
    const std::shared_ptr<void>& registration, std::uint64_t account,
    const Json& profile, network::PacketHandlingResult result) {
  auto lock = LockPlayers();
  auto connection = std::static_pointer_cast<OnlineConnection>(registration);
  connection->account = account;
  connection->profile = profile;
  if (connection->kick) return {{}, true};
  result.responses.insert(result.responses.end(), connection->messages.begin(), connection->messages.end());
  connection->messages.clear();
  return result;
}
void OperationsService::Impl::Kick(std::uint64_t account) {
  for (const auto& weak : online)
    if (auto entry = weak.lock(); entry && entry->account == account) entry->kick = true;
}
}  // namespace bubble_fighter::server::operations
