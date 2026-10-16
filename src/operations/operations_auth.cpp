#include "operations/operations_internal.h"

#include <algorithm>
#include <iostream>
#include <regex>
#include "features/authentication/credential_crypto.h"
#include "features/shop/shop_catalog.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_commands.h"
#include "protocol/auth/auth_json.h"

namespace bubble_fighter::server::operations {
using protocol::auth::Action;
using features::authentication::AccountAuthentication;
using features::authentication::CredentialCrypto;
namespace {
Json Bytes(const auto& value) {
  return SqlBinary({reinterpret_cast<const char*>(value.data()), value.size()});
}
void Password(const std::string& value) {
  if (value.size() < 6 || value.size() > 18 || value.find('\0') != value.npos)
    throw std::invalid_argument("Password must contain 6 to 18 UTF-8 bytes.");
  (void)Utf16(value);
}
std::string QQ(const Json& input) {
  const auto qq = input.value("qq", std::string{});
  if (!qq.empty() && !std::regex_match(qq, std::regex("[1-9][0-9]{4,10}")))
    throw std::invalid_argument("QQ must contain 5 to 11 digits.");
  return qq;
}
std::vector<unsigned int> Version(const std::string& value) {
  if (!std::regex_match(value, std::regex("[0-9]{1,5}\\.[0-9]{1,5}\\.[0-9]{1,5}")))
    throw std::invalid_argument("Expected a three-part launcher version.");
  std::vector<unsigned int> parts;
  for (std::size_t start = 0; start < value.size();) {
    auto end = value.find('.', start);
    parts.push_back(std::stoul(value.substr(start, end - start)));
    if (end == value.npos) break;
    start = end + 1;
  }
  return parts;
}
void CheckVersion(const Json& settings, const std::string& version) {
  if (!settings.value("launcher_enforce", false)) return;
  const auto minimum = settings.value("launcher_min_version", std::string{});
  if (minimum.empty() || Version(version) < Version(minimum))
    throw std::invalid_argument("Please update the KR1304 launcher.");
}
}  // namespace

Json OperationsService::Impl::Profile(MYSQL* db, std::uint64_t account, bool include_items) {
  const auto rows = Query(db,
      "SELECT a.AccountId id,a.Username username,a.IsDisabled disabled,"
      "HEX(c.NameUtf16) nickname,c.CharacterType char_id,c.ColorIndex look,"
      "c.Level level,c.Experience exp,c.Lucci lucci,c.Cash cash,"
      "HEX(c.BattleRecords) battle,IFNULL(o.QQ,'') qq,IFNULL(o.LastIp,'') last_ip,"
      "IFNULL(o.LauncherVersion,'') launcher_version,IFNULL(o.Whitelisted,0) whitelisted "
      "FROM Accounts a LEFT JOIN Characters c ON c.AccountId=a.AccountId "
      "LEFT JOIN OperationAccounts o ON o.AccountId=a.AccountId WHERE a.AccountId=?",
      {account});
  if (rows.empty()) throw std::invalid_argument("Account does not exist.");
  auto result = rows[0];
  result["nickname"] = result["nickname"].is_null() ? "" :
      NameText(result["nickname"].get<std::string>());
  result["character_name"] = result["char_id"].is_null() ? "" :
      CharacterName(result["char_id"].get<std::uint32_t>());
  result["wallet"] = {{"lucci", result["lucci"].is_null() ? Json(0) : result["lucci"]},
                      {"cash", result["cash"].is_null() ? Json(0) : result["cash"]}};
  result["stats"] = result["battle"].is_null() ? Json::object() :
      Statistics(Unhex(result["battle"].get<std::string>()));
  result.erase("battle"); result.erase("lucci"); result.erase("cash");
  if (!include_items) return result;
  result["items"] = Query(db, "SELECT i.ItemId item_id,i.Quantity quantity,"
      "i.UsageKind `usage`,i.ExpiresAtUnixSeconds expires_at,i.Revision revision,"
      "i.IsEquipped equipped FROM InventorySlots i JOIN Characters c "
      "ON c.CharacterId=i.CharacterId WHERE c.AccountId=? ORDER BY i.ItemId", {account});
  for (auto& item : result["items"]) {
    const auto id = item["item_id"].get<std::uint32_t>();
    item["name"] = ItemName(id);
    const auto* definition = features::shop::ShopCatalog::FindItem(id);
    item["main_category"] = definition ? definition->main_category : 0;
    item["sub_category"] = definition ? definition->sub_category : 0;
    DescribeCategory(item);
    item["duration_days"] = item["expires_at"].is_null() ? 36500 :
        (item["expires_at"].get<std::uint64_t>() > Now() ?
         (item["expires_at"].get<std::uint64_t>() - Now() + 86399) / 86400 : 0);
  }
  return result;
}

Json OperationsService::Impl::Verify(MYSQL* db, const Json& input, std::string_view peer) {
  const auto username = AccountAuthentication::NormalizeUsername(input.value("username", ""));
  const auto password = input.value("password", std::string{});
  if (password.empty() || password.size() > 1024) throw std::invalid_argument("Invalid credentials.");
  const auto now = std::chrono::steady_clock::now();
  if (!limiter.Allow(peer, Action::kLogin, now)) throw std::invalid_argument("Too many attempts.");
  persistence::MySqlAccountStore store(database);
  const auto account = store.FindAccount(username);
  if (!account || account->is_disabled ||
      !CredentialCrypto::VerifyPassword(password, account->password)) {
    limiter.RecordLoginFailure(peer, now);
    throw std::invalid_argument("Invalid credentials.");
  }
  if (Banned(db, account->account_id, peer)) throw std::invalid_argument("Account or address is banned.");
  limiter.RecordLoginSuccess(peer);
  return Profile(db, account->account_id);
}

Json OperationsService::Impl::Register(MYSQL* db, const Json& input, bool administrative) {
  auto settings = Settings(db);
  if (!administrative && !settings.value("register_open", true))
    throw std::invalid_argument("Registration is closed.");
  const auto raw = input.value("username", std::string{});
  if (raw.size() < 6 || raw.size() > 18) throw std::invalid_argument("Account must contain 6 to 18 characters.");
  const auto username = AccountAuthentication::NormalizeUsername(raw);
  const auto password = input.value("password", std::string{});
  Password(password);
  const auto qq = QQ(input);
  if (!administrative && settings.value("register_require_qq", false) && qq.empty())
    throw std::invalid_argument("QQ binding is required.");
  const auto nickname = input.value("nickname", input.value("characterName", std::string{}));
  const auto name = nickname.empty() ? std::string{} : NameBytes(nickname);
  const auto verifier = CredentialCrypto::HashPassword(password);
  // The caller owns the transaction, including starter inventory and audit.
  Execute(db, "INSERT INTO Accounts (Username,PasswordSalt,PasswordHash,PasswordIterations) "
      "VALUES (?,?,?,?)", {username, Bytes(verifier.salt), Bytes(verifier.hash), verifier.iterations});
  const auto account = mysql_insert_id(db);
  Execute(db, "INSERT INTO OperationAccounts (AccountId,QQ) VALUES (?,?)",
      {account, qq.empty() ? Json(nullptr) : Json(qq)});
  if (!name.empty()) {
    Execute(db, "INSERT INTO Characters (AccountId,NameUtf16,CharacterType,ColorIndex,"
        "Level,Lucci,Cash) VALUES (?,?,27,2,?,?,?)",
        {account, SqlBinary(name), Bounded(settings, "register_level", 1, 1),
         Bounded(settings, "register_lucci"), Bounded(settings, "register_cash")});
    Grant(db, account, settings.value("register_items", Json::array()));
  } else if (!settings.value("register_items", Json::array()).empty()) {
    throw std::invalid_argument("A character name is required for starter items.");
  }
  return Profile(db, account);
}

std::string OperationsService::Impl::NewSession(MYSQL* db, Json account, std::string_view purpose) {
  Execute(db, "DELETE FROM OperationSessions WHERE ExpiresAt<=?", {Now()});
  const auto token = CredentialCrypto::GenerateTicket();
  Execute(db, "INSERT INTO OperationSessions (TokenHash,AccountId,Purpose,ExpiresAt) VALUES (?,?,?,?)",
      {SqlBinary(Digest(token)), account, std::string(purpose), Now() + 7200});
  return token;
}
Json OperationsService::Impl::Session(MYSQL* db, std::string_view token, std::string_view purpose) {
  if (!CredentialCrypto::IsTicketFormatValid(token)) throw std::invalid_argument("Session expired.");
  const auto rows = Query(db, "SELECT AccountId FROM OperationSessions "
      "WHERE TokenHash=? AND Purpose=? AND ExpiresAt>?", {SqlBinary(Digest(token)), std::string(purpose), Now()});
  if (rows.size() != 1) throw std::invalid_argument("Session expired.");
  if (rows[0]["AccountId"].is_null()) return nullptr;
  auto profile = Profile(db, rows[0]["AccountId"].get<std::uint64_t>());
  if (profile.value("disabled", 0) != 0) throw std::invalid_argument("Account disabled.");
  return profile;
}

Json OperationsService::Impl::Authenticate(MYSQL* db, Action action, const Json& input,
                                          std::string_view peer) {
  const auto settings = Settings(db);
  if (action == Action::kHealth || action == Action::kPolicy)
    return {{"ok", true}, {"settings", settings}, {"notice", config.value("notice", "")}};
  if (action == Action::kRegister) {
    if (!limiter.Allow(peer, action, std::chrono::steady_clock::now()))
      throw std::invalid_argument("Too many attempts.");
    CheckVersion(settings, input.value("launcherVersion", ""));
    persistence::my_sql_commands::Transaction transaction(db);
    auto result = Register(db, input);
    transaction.Commit();
    return {{"ok", true}, {"username", result["username"]}};
  }
  Json account;
  if (action == Action::kLogin || action == Action::kSession || action == Action::kBindQQ) {
    account = Verify(db, input, peer);
    CheckVersion(settings, input.value("launcherVersion", ""));
    if (action == Action::kBindQQ) {
      const auto qq = QQ(input);
      if (qq.empty()) throw std::invalid_argument("QQ is required.");
      if (!account.value("qq", std::string{}).empty() && account["qq"] != qq)
        throw std::invalid_argument("QQ is already bound; contact an administrator.");
      Execute(db, "INSERT INTO OperationAccounts (AccountId,QQ) VALUES (?,?) "
          "ON DUPLICATE KEY UPDATE QQ=VALUES(QQ)", {account["id"], qq});
      account["qq"] = qq;
    }
    if (settings.value("register_require_qq", false) && account.value("qq", "").empty())
      return {{"ok", false}, {"needQQ", true}, {"name", account["username"]},
               {"message", "Please bind QQ before logging in."}};
    Execute(db, "INSERT INTO OperationAccounts (AccountId,LastIp,LauncherVersion) VALUES (?,?,?) "
        "ON DUPLICATE KEY UPDATE LastIp=VALUES(LastIp),LauncherVersion=VALUES(LauncherVersion)",
        {account["id"], std::string(peer), input.value("launcherVersion", "")});
    if (action != Action::kLogin) {
      return {{"ok", true}, {"session", NewSession(db, account["id"], "launcher")},
        {"expiresAt", Now() + 7200}, {"profile", account}};
    }
  } else {
    account = Session(db, input.value("session", ""), "launcher");
    if (account.is_null() || Banned(db, account["id"].get<std::uint64_t>(), peer))
      throw std::invalid_argument("Account or address is banned.");
  }
  const auto id = account["id"].get<std::uint64_t>();
  if (action == Action::kLogin || action == Action::kTicket) {
    CheckVersion(settings, input.value("launcherVersion", ""));
    if (settings.value("register_require_qq", false) && account.value("qq", "").empty())
      throw std::invalid_argument("QQ binding is required.");
    const auto ticket = CredentialCrypto::GenerateTicket();
    Execute(db, "INSERT INTO LoginTickets (TicketHash,AccountId,ExpiresAt) "
        "VALUES (?,?,TIMESTAMPADD(SECOND,300,UTC_TIMESTAMP(6)))", {SqlBinary(Digest(ticket)), id});
    return {{"ok", true}, {"ticket", ticket}, {"username", account["username"]},
             {"expiresAt", Now() + 300}, {"profile", account}};
  }
  if (action == Action::kLogout) {
    Execute(db, "DELETE FROM OperationSessions WHERE TokenHash=?", {SqlBinary(Digest(input.value("session", "")))});
    return {{"ok", true}};
  }
  if (action == Action::kProfile) return {{"ok", true}, {"profile", account}};
  if (action == Action::kCloudGet) {
    Json files = Json::object();
    for (const auto& row : Query(db, "SELECT FileName,Content,ModifiedAt FROM OperationCloudFiles WHERE AccountId=?", {id}))
      files[row["FileName"].get<std::string>()] = {{"bytes", Base64(row["Content"].get<std::string>())},
                                                 {"modifiedAt", row["ModifiedAt"]}};
    return {{"ok", true}, {"files", files}};
  }
  if (action == Action::kCloudPut) {
    const auto files = input.at("files");
    if (!files.is_object() || files.size() > 2) throw std::invalid_argument("Invalid cloud files.");
    persistence::my_sql_commands::Transaction transaction(db);
    for (const auto& [name, file] : files.items()) {
      if (name != "option.ini" && name != "client_display.ini")
        throw std::invalid_argument("Unsupported cloud file.");
      const auto modified = file.at("modifiedAt").get<std::uint64_t>();
      if (!modified || modified > (Now() + 300) * 1000) throw std::invalid_argument("Invalid cloud timestamp.");
      Execute(db, "INSERT INTO OperationCloudFiles (AccountId,FileName,Content,ModifiedAt) VALUES (?,?,?,?) "
          "ON DUPLICATE KEY UPDATE Content=IF(VALUES(ModifiedAt)>ModifiedAt,VALUES(Content),Content),"
          "ModifiedAt=GREATEST(ModifiedAt,VALUES(ModifiedAt))",
          {id, name, SqlBinary(Unbase64(file.at("bytes").get<std::string>())), modified});
    }
    transaction.Commit();
    return {{"ok", true}};
  }
  throw std::invalid_argument("Unsupported authentication action.");
}

protocol::auth::AuthResponse OperationsService::Authenticate(
    const protocol::auth::AuthRequest& request, std::string_view peer,
    const features::authentication::LauncherPolicy& policy) {
  auto lock = LockPlayers();
  protocol::auth::AuthResponse response;
  response.version = request.version;
  try {
    auto db = impl_->Db();
    Json input = request.version == 2 ? protocol::auth::ParseBoundedJson(request.json) :
        Json{{"username", request.username}, {"password", request.password},
             {"characterName", request.character_name}};
    auto result = impl_->Authenticate(db.Handle(), request.action, input, peer);
    if (request.action == Action::kPolicy) {
      result["modes"] = policy.modes; result["crazyModes"] = policy.crazy_modes;
      response.policy = true; response.modes = policy.modes; response.crazy_modes = policy.crazy_modes;
    }
    if (!result.value("ok", false) && request.version == 1)
      throw std::invalid_argument(result.value("message", "Authentication refused."));
    response.json = result.dump();
    response.username = result.contains("ticket") ? result.value("username", "") : "";
    response.ticket = result.value("ticket", "");
    response.expires_at = result.value("expiresAt", std::uint64_t{0});
  } catch (const std::exception& error) {
    if (!dynamic_cast<const std::invalid_argument*>(&error))
      std::cerr << "Launcher authentication operation failed (action "
                << static_cast<unsigned int>(request.action) << "): "
                << error.what() << '\n';
    response.status = protocol::auth::Status::kError;
    response.error = protocol::auth::ErrorCode::kBadRequest;
    response.message = dynamic_cast<const std::invalid_argument*>(&error) ? error.what() :
        "Operation unavailable; check server logs.";
    response.json = Json{{"ok", false}, {"error", response.message}}.dump();
  }
  return response;
}
}  // namespace bubble_fighter::server::operations
