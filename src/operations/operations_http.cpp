#include "operations/operations_internal.h"

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <iostream>
#include "features/authentication/credential_crypto.h"
#include "features/characters/character_creation.h"
#include "features/chat/scene_chat.h"
#include "features/inventory/item_catalog.h"
#include "features/shop/shop_catalog.h"
#include "features/shop/shop_list.h"
#include "persistence/my_sql_commands.h"
#include "protocol/auth/auth_json.h"

namespace bubble_fighter::server::operations {
namespace {
struct HttpError : std::invalid_argument {
  int status;
  HttpError(int code, const char* text) : std::invalid_argument(text), status(code) {}
};
void CheckMethod(const httplib::Request& request) {
  static const std::vector<std::pair<std::regex, std::set<std::string>>> routes{
    {std::regex("/api/auth/(login|logout)"), {"POST"}},
    {std::regex("/api/(auth/me|meta|overview|online(/players)?|catalog/schema|audit|qq-numbers)"), {"GET"}},
    {std::regex("/api/online/[0-9]+/kick"), {"POST"}},
    {std::regex("/api/settings"), {"GET","PATCH"}},
    {std::regex("/api/catalog"), {"GET"}},
    {std::regex("/api/catalog/[0-9]+"), {"GET","PATCH"}},
    {std::regex("/api/catalog/[0-9]+/sale"), {"POST"}},
    {std::regex("/api/shop/rebuild"), {"POST"}},
    {std::regex("/api/accounts"), {"GET","POST"}},
    {std::regex("/api/accounts/[0-9]+"), {"GET","PATCH"}},
    {std::regex("/api/accounts/[0-9]+/wallet"), {"PATCH"}},
    {std::regex("/api/accounts/[0-9]+/items"), {"POST"}},
    {std::regex("/api/accounts/[0-9]+/items/[0-9]+"), {"PATCH","DELETE"}},
    {std::regex("/api/(accounts/bulk/items|accounts/reclaim)"), {"POST"}},
    {std::regex("/api/match-stats/bulk"), {"PATCH"}},
    {std::regex("/api/match-stats"), {"GET"}},
    {std::regex("/api/match-stats/[0-9]+"), {"GET","PATCH"}},
    {std::regex("/api/(bans|announce|gm/admins|gm/ip-allowlist|qq-whitelist)"), {"GET","POST"}},
    {std::regex("/api/(bans/[0-9]+|gm/admins/[0-9]+|gm/ip-allowlist/.+|qq-numbers/bind/[0-9]+|qq-whitelist/[0-9]+)"), {"DELETE"}},
    {std::regex("/api/(qq-numbers/bind|qq-whitelist/bulk|bot/qq-members)"), {"POST"}}
  };
  for (const auto& [pattern, methods] : routes) {
    if (!std::regex_match(request.path, pattern)) continue;
    if (!methods.contains(request.method)) throw HttpError(405, "Method not allowed.");
    return;
  }
  throw HttpError(404, "Unsupported KR management operation.");
}
std::string Cookie(const httplib::Request& request) {
  std::istringstream cookies(request.get_header_value("Cookie"));
  std::string part;
  while (std::getline(cookies, part, ';')) {
    const auto first = part.find_first_not_of(' ');
    if (first != part.npos && part.substr(first).starts_with("cnbfsf_gm_session="))
      return part.substr(first + 18);
  }
  return {};
}
void SetCookie(httplib::Response& response, const std::string& token) {
  response.set_header("Set-Cookie", "cnbfsf_gm_session=" + token +
      "; HttpOnly; SameSite=Strict; Path=/api; Max-Age=" + (token.empty() ? "0" : "7200"));
}
bool EqualSecret(const std::string& left, const std::string& right) {
  if (right.empty()) return false;
  const auto a = Digest(left), b = Digest(right);
  unsigned int difference = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    difference |= static_cast<unsigned char>(a[i] ^ b[i]);
  return difference == 0;
}
Json Page(const httplib::Request& request, Json items) {
  const auto number = [&](const char* key, std::uint32_t fallback, std::uint32_t maximum) {
    if (!request.has_param(key)) return fallback;
    const auto value = request.get_param_value(key);
    if (!std::regex_match(value, std::regex("[0-9]{1,7}"))) throw std::invalid_argument("Invalid pagination.");
    const auto parsed = std::stoul(value);
    if (parsed > maximum) throw std::invalid_argument("Pagination limit exceeded.");
    return static_cast<std::uint32_t>(parsed);
  };
  const auto page = number("page", 0, 100000), size = number("page_size", 30, 2000);
  if (!size) throw std::invalid_argument("Page size must be positive.");
  const auto query = request.get_param_value("q");
  if (!query.empty()) std::erase_if(items.get_ref<Json::array_t&>(), [&](const auto& item) {
    return item.dump().find(query) == std::string::npos;
  });
  const auto total = items.size();
  const auto start = (std::min)(total, static_cast<std::size_t>(page) * size);
  const auto end = (std::min)(total, start + size);
  return {{"items", Json(items.begin() + start, items.begin() + end)},
           {"total", total}, {"page", page}, {"page_size", size}};
}
void Redact(Json& value) {
  if (value.is_array()) for (auto& item : value) Redact(item);
  if (!value.is_object()) return;
  for (auto& [key, item] : value.items()) {
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (lower.find("password") != lower.npos || lower.find("token") != lower.npos ||
        lower.find("session") != lower.npos) item = "[redacted]";
    else Redact(item);
  }
}
Json Targets(MYSQL* db, const Json& input) {
  const auto targets = input.at("targets");
  if (targets == "all") {
    Json result = Json::array();
    for (const auto& row : Query(db, "SELECT AccountId FROM Characters ORDER BY AccountId LIMIT 1001"))
      result.push_back(row["AccountId"]);
    if (result.size() > 1000) throw std::invalid_argument("Select at most 1000 accounts per transaction.");
    return result;
  }
  if (!targets.is_array() || targets.empty() || targets.size() > 1000)
    throw std::invalid_argument("Select 1 to 1000 accounts.");
  std::set<std::uint64_t> unique;
  for (const auto& value : targets) {
    const auto id = value.get<std::uint64_t>();
    if (!id) throw std::invalid_argument("Invalid target.");
    unique.insert(id);
  }
  return Json(unique);
}
std::uint32_t Address(std::string_view value) {
  return network::TcpServer::ParseListenAddress(value);
}
bool InNetwork(std::string_view address, std::string_view network) {
  const auto slash = network.find('/');
  const auto host = Address(address), base = Address(network.substr(0, slash));
  if (slash == network.npos) return host == base;
  const auto bits = std::string(network.substr(slash + 1));
  if (!std::regex_match(bits, std::regex("[0-9]{1,2}")) || std::stoul(bits) > 32)
    throw std::invalid_argument("Invalid CIDR.");
  const auto count = std::stoul(bits);
  const std::uint32_t mask = count ? 0xffffffffu << (32 - count) : 0;
  return (host & mask) == (base & mask);
}
}  // namespace

bool OperationsService::Impl::AllowIp(MYSQL* db, std::string_view peer) {
  if (peer == "127.0.0.1" || peer == "::1") return true;
  const auto entries = Query(db, "SELECT Entry FROM OperationIpAllowlist");
  if (entries.empty()) return true;
  for (const auto& entry : entries)
    if (InNetwork(peer, entry["Entry"].get<std::string>())) return true;
  return false;
}
std::string OperationsService::Impl::Authorize(MYSQL* db, const httplib::Request& request) {
  if (!AllowIp(db, request.remote_addr)) throw HttpError(403, "Address is not allowed.");
  const auto token = Cookie(request);
  auto result = Query(db, "SELECT s.AccountId,s.Purpose FROM OperationSessions s "
      "LEFT JOIN Accounts a ON a.AccountId=s.AccountId LEFT JOIN OperationAdmins g ON g.AccountId=a.AccountId "
      "WHERE s.TokenHash=? AND s.ExpiresAt>? AND (s.Purpose='bootstrap' OR "
      "(s.Purpose='gm' AND a.IsDisabled=0 AND g.AccountId IS NOT NULL))",
      {SqlBinary(Digest(token)), Now()});
  if (result.empty()) throw HttpError(401, "Please sign in.");
  if (!result[0]["AccountId"].is_null() && Banned(db, result[0]["AccountId"].get<std::uint64_t>(), request.remote_addr))
    throw HttpError(403, "Account is banned.");
  return result[0]["Purpose"] == "bootstrap" ? "bootstrap" :
      "account:" + std::to_string(result[0]["AccountId"].get<std::uint64_t>());
}
void OperationsService::Impl::Audit(MYSQL* db, std::string_view actor,
    const httplib::Request& request, const Json& input, const Json& before,
    const Json& after, bool ok) {
  auto redacted = input;
  Redact(redacted);
  Execute(db, "INSERT INTO OperationAudit (Actor,Action,AccountId,Request,BeforeValue,AfterValue,Ok) "
      "VALUES (?,?,?,?,?,?,?)", {std::string(actor), request.method + " " + request.path,
        input.value("account_id", Json(nullptr)), redacted.dump(),
        before.is_null() ? Json(nullptr) : Json(before.dump()),
        after.is_null() ? Json(nullptr) : Json(after.dump()), ok});
}
void OperationsService::Impl::Routes() {
  gm.set_payload_max_length(262144);
  gm.set_read_timeout(10);
  gm.set_write_timeout(10);
  gm.set_default_headers({{"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"},
      {"X-Frame-Options", "DENY"}, {"Referrer-Policy", "no-referrer"}});
  const auto handler = [this](const httplib::Request& request, httplib::Response& response) {
    std::lock_guard lock(gate);
    Json input = Json::object();
    std::string actor = "unauthenticated";
    const bool writing = request.method != "GET";
    auto previous_shop = features::shop::ShopList::Active();
    std::vector<std::pair<std::shared_ptr<OnlineConnection>, OnlineConnection>> saved_connections;
    for (const auto& weak : online)
      if (auto connection = weak.lock()) saved_connections.emplace_back(connection, *connection);
    try {
      CheckMethod(request);
      auto db = Db();
      if (writing) {
        if (request.get_header_value("Content-Type").find("application/json") != 0)
          throw HttpError(415, "JSON content type is required.");
        const auto origin = request.get_header_value("Origin");
        if ((!origin.empty() && origin != "http://" + request.get_header_value("Host") &&
             origin != "https://" + request.get_header_value("Host")) ||
            request.get_header_value("Sec-Fetch-Site") == "cross-site")
          throw HttpError(403, "Cross-site request refused.");
        if (!request.body.empty()) input = protocol::auth::ParseBoundedJson(request.body);
        if (!input.is_object()) throw std::invalid_argument("Expected a JSON object.");
      }
      if (request.path == "/api/auth/login" && request.method == "POST") {
        if (!AllowIp(db.Handle(), request.remote_addr)) throw HttpError(403, "Address is not allowed.");
        if (!limiter.Allow(request.remote_addr, protocol::auth::Action::kLogin, std::chrono::steady_clock::now()))
          throw HttpError(429, "Too many attempts.");
        std::string token;
        if (input.contains("token")) {
          if (!EqualSecret(input.value("token", ""), config.value("bootstrap_token", ""))) {
            limiter.RecordLoginFailure(request.remote_addr, std::chrono::steady_clock::now());
            throw HttpError(401, "Invalid initialization token.");
          }
          actor = "bootstrap";
          token = NewSession(db.Handle(), nullptr, "bootstrap");
        } else {
          const auto account = Verify(db.Handle(), input, request.remote_addr);
          if (Query(db.Handle(), "SELECT AccountId FROM OperationAdmins WHERE AccountId=?", {account["id"]}).empty())
            throw HttpError(403, "This game account has no management permission.");
          actor = "account:" + std::to_string(account["id"].get<std::uint64_t>());
          token = NewSession(db.Handle(), account["id"], "gm");
        }
        SetCookie(response, token);
        Audit(db.Handle(), actor, request, input, nullptr, nullptr, true);
        response.set_content(Json{{"ok", true}, {"data", {{"actor", actor}, {"authenticated", true}}}}.dump(), "application/json");
        return;
      }
      if (request.path.starts_with("/api/bot/")) {
        if (!EqualSecret(request.get_header_value("Authorization"), "Bearer " + config.value("bot_token", "")) ||
            config.value("bot_token", "").empty()) throw HttpError(401, "Invalid bot token.");
        actor = "qq-bot";
        if (request.path != "/api/bot/qq-members" || request.method != "POST")
          throw HttpError(404, "Unknown bot endpoint.");
        const auto members = input.at("members");
        if (!members.is_array() || members.size() > 1000) throw std::invalid_argument("Too many QQ members.");
        persistence::my_sql_commands::Transaction transaction(db.Handle());
        for (const auto& member : members) {
          const auto qq = member.at("qq").get<std::string>();
          if (!std::regex_match(qq, std::regex("[1-9][0-9]{4,10}"))) throw std::invalid_argument("Invalid QQ.");
          Execute(db.Handle(), "INSERT INTO OperationQQMembers (QQ,Nickname,Card,Role) VALUES (?,?,?,?) "
              "ON DUPLICATE KEY UPDATE Nickname=VALUES(Nickname),Card=VALUES(Card),Role=VALUES(Role)",
              {qq, member.value("nickname", ""), member.value("card", ""), member.value("role", "member")});
        }
        Audit(db.Handle(), actor, request, input, nullptr, {{"members", members.size()}}, true);
        transaction.Commit();
        response.set_content("{\"ok\":true}", "application/json");
        return;
      }
      actor = Authorize(db.Handle(), request);
      if (request.path == "/api/auth/me") {
        response.set_content(Json{{"ok", true}, {"data", {{"actor", actor}, {"authenticated", true}}}}.dump(), "application/json");
        return;
      }
      if (request.path == "/api/auth/logout" && writing) {
        Execute(db.Handle(), "DELETE FROM OperationSessions WHERE TokenHash=?", {SqlBinary(Digest(Cookie(request)))});
        SetCookie(response, "");
        response.set_content("{\"ok\":true}", "application/json");
        return;
      }
      Json before = nullptr;
      std::smatch match;
      std::unique_ptr<persistence::my_sql_commands::Transaction> transaction;
      if (writing) transaction = std::make_unique<persistence::my_sql_commands::Transaction>(db.Handle());
      if (writing && std::regex_search(request.path, match, std::regex("^/api/accounts/([0-9]+)(/|$)"))) {
        const auto id = std::stoull(match[1]);
        if (Query(db.Handle(), "SELECT AccountId FROM Characters WHERE AccountId=? FOR UPDATE", {id}).empty())
          throw HttpError(409, "Account has no character.");
        before = Profile(db.Handle(), id); input["account_id"] = id;
      }
      const auto output = Api(db.Handle(), request, response, input, actor);
      if (writing) {
        Json after = output;
        if (input.contains("account_id") && !before.is_null())
          after = Profile(db.Handle(), input["account_id"].get<std::uint64_t>());
        Audit(db.Handle(), actor, request, input, before, after, true);
        transaction->Commit();
      }
      response.set_content(Json{{"ok", true}, {"data", output}}.dump(), "application/json");
    } catch (const std::exception& error) {
      features::shop::ShopList::SetActive(previous_shop);
      for (auto& [connection, saved] : saved_connections) *connection = std::move(saved);
      std::cerr << "WebGM operation failed: " << error.what() << '\n';
      const auto* http = dynamic_cast<const HttpError*>(&error);
      const bool client = dynamic_cast<const std::invalid_argument*>(&error) != nullptr;
      response.status = http ? http->status : client ? 400 : 503;
      const std::string message = client ? error.what() : "Operation unavailable; check schema and server logs.";
      response.set_content(Json{{"ok", false}, {"error", message}}.dump(), "application/json");
      if (writing) try {
        auto db = Db();
        Audit(db.Handle(), actor, request, input, nullptr, nullptr, false);
      } catch (...) {}
    }
  };
  gm.Get("/api/.*", handler); gm.Post("/api/.*", handler);
  gm.Patch("/api/.*", handler); gm.Delete("/api/.*", handler);
  if (config.contains("web_root") &&
      !gm.set_mount_point("/", config["web_root"].get<std::string>()))
    throw std::invalid_argument("WebGM static directory is missing; build webgm-ui first.");
}

Json OperationsService::Impl::Api(MYSQL* db, const httplib::Request& request,
    httplib::Response&, const Json& input, std::string_view actor) {
  const auto& path = request.path;
  const bool get = request.method == "GET", remove = request.method == "DELETE";
  std::smatch match;
  if (get && path == "/api/meta") return {
    {"game", "KR1304"}, {"currencies", {"lucci", "cash"}},
    {"capabilities", {{"couples", false}, {"pets", false}, {"third_currency", false},
      {"cloud", true}, {"catalog_definitions_editable", false}, {"announce_modes", {"chat"}}}},
    {"limits", {{"integer_max", 0x7fffffff}, {"cloud_file_bytes", 65536}, {"page_size", 2000}}},
    {"characters", CharacterOptions()},
    {"categories", CatalogCategories()},
    {"money_types", Json::array({{{"value",1},{"name","Lucci"}},{{"value",9},{"name","Cash"}}})},
    {"announce", {{"modes", {"chat"}}, {"positions", Json::array({{{"value","chat"},{"label","系统聊天"}}})}}}};
  if (get && (path == "/api/online" || path == "/api/online/players" || path == "/api/overview")) {
    Json players = Json::array();
    std::set<std::uint64_t> accounts;
    for (const auto& weak : online) if (const auto entry = weak.lock();
        entry && entry->account && !entry->kick && accounts.insert(entry->account).second) {
      auto profile = entry->profile; profile["account"] = entry->account;
      players.push_back(profile);
    }
    return {{"online", players.size()}, {"items", players}, {"code", {{"stale", false}}}};
  }
  if (!get && std::regex_match(path, match, std::regex("/api/online/([0-9]+)/kick"))) {
    const auto id = std::stoull(match[1]); Kick(id); return {{"queued", true}};
  }
  if (path == "/api/settings") {
    if (get) return Settings(db);
    auto settings = Settings(db);
    for (const auto& [key, value] : input.items()) {
      if (!settings.contains(key)) throw std::invalid_argument("Unknown operational setting.");
      settings[key] = value;
    }
    for (const auto* key : {"register_open","register_require_qq","launcher_enforce"})
      if (!settings[key].is_boolean()) throw std::invalid_argument("Expected boolean setting.");
    for (const auto* key : {"register_level","register_cash","register_lucci"})
      (void)Bounded(settings, key, 0, std::string_view(key) == "register_level" ? 1 : 0);
    for (const auto* key : {"launcher_min_version","launcher_latest_version"})
      if (!settings[key].is_string() || (!settings[key].get<std::string>().empty() &&
          !std::regex_match(settings[key].get<std::string>(), std::regex("[0-9]{1,5}\\.[0-9]{1,5}\\.[0-9]{1,5}"))))
        throw std::invalid_argument("Invalid launcher version.");
    if (settings.value("launcher_enforce", false) && settings.value("launcher_min_version", "").empty())
      throw std::invalid_argument("Minimum launcher version is required.");
    if (!settings["register_items"].is_array() || settings["register_items"].size() > 1024)
      throw std::invalid_argument("Invalid starter items.");
    for (const auto& item : settings["register_items"]) {
      if (!item.is_array() || item.size() != 3) throw std::invalid_argument("Expected starter [item,quantity,days].");
      const auto* definition = features::inventory::ItemCatalog::Find(item[0].get<std::uint32_t>());
      if (!definition) throw std::invalid_argument("Unknown starter item.");
      const auto quantity = item[1].get<std::int64_t>(), days = item[2].get<std::int64_t>();
      if (quantity < 1 || quantity > 0x7fffffff || days < 1 ||
          (days > 10000 && days != 36500)) throw std::invalid_argument("Invalid starter quantity or duration.");
    }
    Execute(db, "UPDATE OperationSettings SET Document=? WHERE SettingId=1", {settings.dump()});
    return settings;
  }
  if (path == "/api/catalog/schema" && get)
    return {{"kinds", Json::array()}, {"mains", Json::array()}, {"next_ids", Json::object()}};
  if (path == "/api/catalog" && get) {
    auto catalog = Catalog(db);
    const auto main = request.get_param_value("main"), sub = request.get_param_value("sub");
    const auto sale = request.get_param_value("on_sale");
    if (!sale.empty() && sale != "0" && sale != "1")
      throw std::invalid_argument("Expected on_sale=0 or 1.");
    std::erase_if(catalog.get_ref<Json::array_t&>(), [&](const auto& row) {
      return (!main.empty() && row["main"] != main) || (!sub.empty() && row["sub"] != sub) ||
          (!sale.empty() && row["on_sale"].template get<bool>() != (sale == "1"));
    });
    return Page(request, std::move(catalog));
  }
  if (std::regex_match(path, match, std::regex("/api/catalog/([0-9]+)(/sale)?"))) {
    const auto id = std::stoul(match[1]);
    auto catalog = Catalog(db);
    const auto found = std::find_if(catalog.begin(), catalog.end(), [id](const auto& row) { return row["id"] == id; });
    if (found == catalog.end()) throw HttpError(404, "Unknown KR item.");
    if (get) return *found;
    if (remove) throw std::invalid_argument("Client item definitions are read-only.");
    auto row = *found;
    for (const auto& [key, value] : input.items()) {
      if (key != "name" && key != "on_sale" && key != "offers" && key != "main_category" &&
          key != "sub_category" && key != "display_order" && key != "sync_shop")
        throw std::invalid_argument("Client definitions are read-only.");
      row[key] = value;
    }
    DescribeCategory(row);
    Execute(db, "INSERT INTO OperationShopEntries (ItemId,Document) VALUES (?,?) "
        "ON DUPLICATE KEY UPDATE Document=VALUES(Document)", {id, row.dump()});
    auto result = Rebuild(db); result["item"] = row; return result;
  }
  if (path == "/api/shop/rebuild" && !get) return Rebuild(db);
  if (path == "/api/accounts" && !get) return Register(db, input, true);
  if (get && (path == "/api/accounts" || path == "/api/match-stats")) {
    const auto pagination = Page(request, Json::array());
    const auto page = pagination["page"].get<std::uint32_t>();
    const auto size = pagination["page_size"].get<std::uint32_t>();
    const auto query = request.get_param_value("q");
    if (query.size() > 256) throw std::invalid_argument("Search is too long.");
    const auto name = Utf16(query);
    const std::string name_bytes(reinterpret_cast<const char*>(name.data()), name.size()*2);
    const std::string filter = " FROM Accounts a LEFT JOIN Characters c ON c.AccountId=a.AccountId "
        "LEFT JOIN OperationAccounts o ON o.AccountId=a.AccountId WHERE "
        "(?='' OR INSTR(CONVERT(a.Username USING utf8mb4) COLLATE utf8mb4_bin,?)>0 "
        "OR CAST(a.AccountId AS CHAR CHARACTER SET utf8mb4)=? "
        "OR INSTR(c.NameUtf16,?)>0 OR CONVERT(o.QQ USING utf8mb4) COLLATE utf8mb4_bin=?)";
    std::vector<Json> values{query,query,query,SqlBinary(name_bytes),query};
    const auto total = Query(db, "SELECT COUNT(*) total" + filter, values)[0]["total"];
    values.push_back(size); values.push_back(static_cast<std::uint64_t>(page)*size);
    Json accounts = Json::array();
    for (const auto& row : Query(db, "SELECT a.AccountId" + filter + " ORDER BY a.AccountId DESC LIMIT ? OFFSET ?", values)) {
      auto profile = Profile(db, row["AccountId"].get<std::uint64_t>(), false);
      if (path == "/api/match-stats") {
        auto stats = profile["stats"]; stats["account_id"] = profile["id"];
        stats["username"] = profile["username"]; stats["nickname"] = profile["nickname"]; accounts.push_back(stats);
      } else accounts.push_back(profile);
    }
    return {{"items",accounts},{"total",total},{"page",page},{"page_size",size}};
  }
  if (std::regex_match(path, match, std::regex("/api/accounts/([0-9]+)(/wallet|/items(?:/([0-9]+))?)?"))) {
    const auto id = std::stoull(match[1]); const auto suffix = match[2].str();
    const auto before = Profile(db, id);
    if (get) return before;
    if (remove && suffix.empty()) throw std::invalid_argument("Use account bans to preserve character history.");
    if (suffix == "/wallet") {
      const auto lucci = Bounded(input, "lucci", before["wallet"]["lucci"].get<std::uint32_t>());
      const auto cash = Bounded(input, "cash", before["wallet"]["cash"].get<std::uint32_t>());
      const auto affected = Execute(db, "UPDATE Characters SET Lucci=?,Cash=? WHERE AccountId=? AND Lucci=? AND Cash=?",
          {lucci, cash, id, before["wallet"]["lucci"], before["wallet"]["cash"]});
      if (affected != 1 && (lucci != before["wallet"]["lucci"] || cash != before["wallet"]["cash"]))
        throw HttpError(409, "Wallet revision conflict.");
    } else if (suffix == "/items") Grant(db, id, input.at("items"));
    else if (!suffix.empty()) {
      auto change = input; change["remove"] = remove;
      Bag(db, id, std::stoul(match[3]), change);
    } else {
      const auto type = Bounded(input, "char_id", before["char_id"].is_null() ? 27 : before["char_id"].get<std::uint32_t>());
      const auto look = Bounded(input, "look", before["look"].is_null() ? 2 : before["look"].get<std::uint32_t>());
      if (!features::characters::CharacterCreation::SupportsSelection(type, look))
        throw std::invalid_argument("Unsupported KR character/color pair.");
      Execute(db, "UPDATE Characters SET NameUtf16=?,CharacterType=?,ColorIndex=?,Level=?,Experience=? "
          "WHERE AccountId=? AND NameUtf16=? AND Level=? AND Experience=?",
          {SqlBinary(NameBytes(input.value("nickname", before.value("nickname", "")))), type, look,
           Bounded(input, "level", before.value("level", 1u), 1), Bounded(input, "exp", before.value("exp", 0u)),
           id, SqlBinary(NameBytes(before.value("nickname", ""))), before["level"], before["exp"]});
      if (input.contains("qq")) {
        const auto qq = input["qq"].get<std::string>();
        if (!qq.empty() && !std::regex_match(qq, std::regex("[1-9][0-9]{4,10}"))) throw std::invalid_argument("Invalid QQ.");
        Execute(db, "INSERT INTO OperationAccounts (AccountId,QQ) VALUES (?,?) "
            "ON DUPLICATE KEY UPDATE QQ=VALUES(QQ)", {id, qq.empty() ? Json(nullptr) : Json(qq)});
      }
    }
    Kick(id); return Profile(db, id);
  }
  if (!get && (path == "/api/accounts/bulk/items" || path == "/api/accounts/reclaim" ||
      path == "/api/match-stats/bulk")) {
    const auto targets = Targets(db, input);
    std::size_t count = 0;
    for (const auto& target : targets) {
      const auto id = target.get<std::uint64_t>();
      if (path == "/api/accounts/bulk/items") Grant(db, id, input.at("items"));
      else if (path == "/api/match-stats/bulk") Stats(db, id, input);
      else for (const auto& item : input.at("items")) {
        const auto profile = Profile(db, id);
        if (std::any_of(profile["items"].begin(), profile["items"].end(),
            [&](const auto& entry) { return entry["item_id"] == item; })) {
          Bag(db, id, item.get<std::uint32_t>(), {{"remove", true}}); ++count;
        }
      }
    }
    return {{"accounts", targets.size()}, {"quantity", count}};
  }
  if (std::regex_match(path, match, std::regex("/api/match-stats/([0-9]+)"))) {
    const auto id = std::stoull(match[1]);
    if (!get) Stats(db, id, input);
    return Profile(db, id)["stats"];
  }
  if (path == "/api/bans" && get)
    return Page(request, Query(db, "SELECT BanId id,Kind kind,Kind label,Target value,Reason reason,"
      "ExpiresAt expires_at,CreatedBy created_by FROM OperationBans ORDER BY BanId DESC LIMIT 10000"));
  if (std::regex_match(path, match, std::regex("/api/bans/([0-9]+)")) && remove) {
    Execute(db, "DELETE FROM OperationBans WHERE BanId=?", {std::stoull(match[1])}); return Json::object();
  }
  if (path == "/api/bans" && !get) {
    const auto days = Bounded(input, "days"), hours = Bounded(input, "hours");
    const auto duration = days * 86400ULL + hours * 3600ULL;
    const auto add = [&](const std::string& kind, const std::string& target) {
      if (target.empty() || (kind != "account" && kind != "ip")) throw std::invalid_argument("Unsupported ban target.");
      if (kind == "ip") (void)Address(target);
      Execute(db, "INSERT INTO OperationBans (Kind,Target,Reason,ExpiresAt,CreatedBy) VALUES (?,?,?,?,?)",
          {kind, target, input.value("reason", ""), duration ? Now() + duration : 0, std::string(actor)});
    };
    if (input.contains("account_id")) {
      const auto id = input["account_id"].get<std::uint64_t>(); const auto profile = Profile(db, id);
      add("account", std::to_string(id)); Kick(id);
      Execute(db, "DELETE FROM LoginTickets WHERE AccountId=?", {id});
      Execute(db, "DELETE FROM OperationSessions WHERE AccountId=?", {id});
      if (input.value("also_ip", false) && !profile.value("last_ip", "").empty()) add("ip", profile["last_ip"]);
    } else {
      const auto kind = input.value("kind", "");
      add(kind, input.value("value", ""));
      for (const auto& weak : online) if (auto entry = weak.lock(); entry &&
          entry->profile.value("ip", "") == input.value("value", "")) entry->kick = true;
    }
    return {{"skipped", Json::array()}};
  }
  if (path == "/api/announce") {
    if (get) return Page(request, Query(db, "SELECT AnnouncementId id,Message message,'chat' mode,"
        "Actor actor,Delivered queued,UNIX_TIMESTAMP(CreatedAt) created_at FROM OperationAnnouncements ORDER BY AnnouncementId DESC LIMIT 1000"));
    if (input.value("mode", "chat") != "chat") throw std::invalid_argument("Only verified system chat is supported.");
    const auto text = Utf16(input.value("message", ""));
    if (!features::chat::SceneChat::ValidText(text)) throw std::invalid_argument("Invalid system chat text (maximum 256 UTF-16 units).");
    const auto packet = features::chat::SceneChat::Notice(text);
    std::size_t delivered = 0;
    for (const auto& weak : online)
      if (auto entry = weak.lock(); entry && entry->account && !entry->kick &&
          entry->messages.size() >= 32) throw std::invalid_argument("Announcement queue is full.");
    for (auto& weak : online) if (auto entry = weak.lock(); entry && entry->account && !entry->kick) {
      if (entry->messages.size() >= 32) throw std::invalid_argument("Announcement queue is full.");
      entry->messages.push_back(packet); ++delivered;
    }
    Execute(db, "INSERT INTO OperationAnnouncements (Message,Actor,Delivered) VALUES (?,?,?)",
        {input["message"], std::string(actor), delivered});
    return {{"queued", delivered}, {"delivered", 0}};
  }
  if (path == "/api/gm/admins") {
    if (get) return {{"items", Query(db, "SELECT g.AccountId account_id,a.Username username,g.Note note "
        "FROM OperationAdmins g JOIN Accounts a ON a.AccountId=g.AccountId")}};
    const auto id = input.at("account_id");
    Execute(db, "INSERT INTO OperationAdmins (AccountId,Note) VALUES (?,?) ON DUPLICATE KEY UPDATE Note=VALUES(Note)",
        {id, input.value("note", "")}); return Json::object();
  }
  if (std::regex_match(path, match, std::regex("/api/gm/admins/([0-9]+)")) && remove) {
    const auto id = std::stoull(match[1]);
    Execute(db, "DELETE FROM OperationAdmins WHERE AccountId=?", {id});
    Execute(db, "DELETE FROM OperationSessions WHERE AccountId=? AND Purpose='gm'", {id});
    return Json::object();
  }
  if (path == "/api/gm/ip-allowlist") {
    if (get) return {{"items", Query(db, "SELECT Entry ip,Entry entry,Note note FROM OperationIpAllowlist")}, {"env", Json::array()}};
    const auto entry = input.value("ip", input.value("entry", ""));
    (void)InNetwork("127.0.0.1", entry);
    Execute(db, "INSERT INTO OperationIpAllowlist (Entry,Note) VALUES (?,?) ON DUPLICATE KEY UPDATE Note=VALUES(Note)",
        {entry, input.value("note", "")}); return Json::object();
  }
  if (path.starts_with("/api/gm/ip-allowlist/") && remove) {
    Execute(db, "DELETE FROM OperationIpAllowlist WHERE Entry=?", {path.substr(21)}); return Json::object();
  }
  if (path == "/api/qq-numbers" && get) return Page(request, Query(db,
      "SELECT q.QQ qq,q.Nickname nickname,q.Card card,q.Role role,a.Username bound_username,"
      "o.AccountId bound_account_id FROM OperationQQMembers q LEFT JOIN OperationAccounts o ON o.QQ=q.QQ "
      "LEFT JOIN Accounts a ON a.AccountId=o.AccountId UNION SELECT o.QQ,'','','member',a.Username,o.AccountId "
      "FROM OperationAccounts o JOIN Accounts a ON a.AccountId=o.AccountId "
      "WHERE o.QQ IS NOT NULL AND o.QQ NOT IN (SELECT QQ FROM OperationQQMembers) LIMIT 10000"));
  if (path == "/api/qq-numbers/bind" && !get) {
    const auto qq = input.at("qq").get<std::string>(); const auto id = input.at("account_id");
    if (!std::regex_match(qq, std::regex("[1-9][0-9]{4,10}"))) throw std::invalid_argument("Invalid QQ.");
    Execute(db, "INSERT INTO OperationAccounts (AccountId,QQ) VALUES (?,?) ON DUPLICATE KEY UPDATE QQ=VALUES(QQ)", {id, qq});
    Kick(id.get<std::uint64_t>()); return Json::object();
  }
  if (std::regex_match(path, match, std::regex("/api/qq-numbers/bind/([0-9]+)")) && remove) {
    const auto id = std::stoull(match[1]);
    Execute(db, "UPDATE OperationAccounts SET QQ=NULL WHERE AccountId=?", {id}); Kick(id); return Json::object();
  }
  if (path == "/api/qq-whitelist" && get) {
    auto rows = Query(db, "SELECT o.AccountId account_id,"
      "a.Username username,HEX(c.NameUtf16) nickname,o.QQ qq,o.Note note,o.AddedBy added_by "
      "FROM OperationAccounts o JOIN Accounts a ON a.AccountId=o.AccountId "
      "LEFT JOIN Characters c ON c.AccountId=a.AccountId WHERE o.Whitelisted=1");
    for (auto& row : rows) row["nickname"] = row["nickname"].is_null() ? "" : NameText(row["nickname"].get<std::string>());
    return Page(request, rows);
  }
  if (path == "/api/qq-whitelist" && !get) {
    const auto id = input.at("account_id");
    const auto profile = Profile(db, id.get<std::uint64_t>());
    if (profile.value("qq", "").empty()) throw std::invalid_argument("Bind QQ before adding the whitelist.");
    Execute(db, "UPDATE OperationAccounts SET Whitelisted=1,Note=?,AddedBy=? WHERE AccountId=?",
        {input.value("note", ""), std::string(actor), id}); return Json::object();
  }
  if (path == "/api/qq-whitelist/bulk" && !get) {
    std::string text = input.value("text", "");
    for (auto& ch : text) if (ch == ',' || ch == ';') ch = ' ';
    std::istringstream tokens(text); std::string token; int added=0, existed=0, missing=0;
    while (tokens >> token) {
      const auto name = Utf16(token);
      const auto found = Query(db, "SELECT o.AccountId,o.Whitelisted FROM OperationAccounts o "
          "JOIN Accounts a ON a.AccountId=o.AccountId LEFT JOIN Characters c ON c.AccountId=o.AccountId "
          "WHERE o.QQ IS NOT NULL AND (a.Username=? OR o.QQ=? OR CAST(o.AccountId AS CHAR)=? OR c.NameUtf16=?)",
          {token, token, token, SqlBinary({reinterpret_cast<const char*>(name.data()), name.size()*2})});
      if (found.size() != 1) { ++missing; continue; }
      if (found[0]["Whitelisted"] == 1) { ++existed; continue; }
      Execute(db, "UPDATE OperationAccounts SET Whitelisted=1,AddedBy=? WHERE AccountId=?",
          {std::string(actor), found[0]["AccountId"]}); ++added;
    }
    return {{"added",added},{"existed",existed},{"missing",missing}};
  }
  if (std::regex_match(path, match, std::regex("/api/qq-whitelist/([0-9]+)")) && remove) {
    Execute(db, "UPDATE OperationAccounts SET Whitelisted=0 WHERE AccountId=?", {std::stoull(match[1])}); return Json::object();
  }
  if (path == "/api/audit" && get) {
    auto rows = Query(db, "SELECT AuditId id,Actor actor,Action action,AccountId account_id,Request request,"
        "BeforeValue before_value,AfterValue after_value,Ok ok,UNIX_TIMESTAMP(CreatedAt) created_at "
        "FROM OperationAudit ORDER BY AuditId DESC LIMIT 10000");
    for (auto& row : rows) for (const auto* key : {"request","before_value","after_value"})
      if (row[key].is_string()) row[key] = Json::parse(row[key].get<std::string>());
    return Page(request, rows);
  }
  throw HttpError(404, "Unsupported KR management operation.");
}
}  // namespace bubble_fighter::server::operations
