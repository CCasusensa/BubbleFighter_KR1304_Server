#include "application/account_command.h"

#include "features/authentication/account_authentication.h"
#include "features/characters/character_creation.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <sddl.h>

#include <charconv>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "protocol/characters/create_user_info_request.h"

namespace bubble_fighter::server::application {
namespace {
class PasswordInput {
 public:
  PasswordInput() {
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original_mode = 0;
    const bool console = GetConsoleMode(input, &original_mode) != 0;
    const auto original_code_page = GetConsoleCP();
    if (console && !SetConsoleMode(input, original_mode & ~ENABLE_ECHO_INPUT)) {
      throw std::runtime_error("Unable to disable password echo.");
    }
    struct RestoreConsole {
      HANDLE input;
      DWORD mode;
      UINT code_page;
      bool active;
      ~RestoreConsole() {
        if (active) {
          SetConsoleMode(input, mode);
          SetConsoleCP(code_page);
          std::cerr << '\n';
        }
      }
    } restore{input, original_mode, original_code_page, console};
    if (console && !SetConsoleCP(CP_UTF8)) {
      throw std::runtime_error("Unable to configure UTF-8 password input.");
    }
    std::cerr << "Password: " << std::flush;
    char value = 0;
    try {
      while (std::cin.get(value) && value != '\n') {
        if (password_.size() == 1024) {
          throw std::invalid_argument("Password exceeds the supported length.");
        }
        password_.push_back(value);
      }
      if (!password_.empty() && password_.back() == '\r') {
        password_.pop_back();
      }
      if (!password_.empty() &&
          MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, password_.data(),
                              static_cast<int>(password_.size()), nullptr,
                              0) == 0) {
        throw std::invalid_argument("Password input must be UTF-8.");
      }
    } catch (...) {
      SecureZeroMemory(password_.data(), password_.size());
      throw;
    }
  }
  ~PasswordInput() { SecureZeroMemory(password_.data(), password_.size()); }
  PasswordInput(const PasswordInput&) = delete;
  PasswordInput& operator=(const PasswordInput&) = delete;
  std::string_view Value() const { return password_; }

 private:
  std::string password_;
};

void WritePrivateTicket(const std::filesystem::path& path,
                        std::string_view ticket) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    throw std::runtime_error("Unable to obtain ticket file owner.");
  }
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<std::uint8_t> information(size);
  const auto read =
      GetTokenInformation(token, TokenUser, information.data(), size, &size);
  CloseHandle(token);
  if (!read) {
    throw std::runtime_error("Unable to read ticket file owner.");
  }
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(
          reinterpret_cast<TOKEN_USER*>(information.data())->User.Sid, &sid)) {
    throw std::runtime_error("Unable to format ticket file owner.");
  }
  const std::wstring sddl =
      std::wstring(L"D:P(A;;FA;;;") + sid + L")(A;;FA;;;SY)(A;;FA;;;BA)";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    throw std::runtime_error("Unable to create ticket file permissions.");
  }
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor,
                                 FALSE};
  const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  LocalFree(descriptor);
  if (file == INVALID_HANDLE_VALUE) {
    throw std::runtime_error(
        "Unable to create ticket file; the path must not already exist.");
  }
  DWORD written = 0;
  const bool saved =
      WriteFile(file, ticket.data(), static_cast<DWORD>(ticket.size()),
                &written, nullptr) &&
      written == ticket.size() && FlushFileBuffers(file);
  CloseHandle(file);
  if (!saved) {
    throw std::runtime_error("Unable to finish writing ticket file.");
  }
}
}  // namespace

namespace {
// Local administration: give an existing account a character without going
// through the client's creation window. The row is written by the same store
// the game uses, so the name/selection rules and the default statistics match
// a character the client would have created itself.
int RunCreateCharacter(int argc, char* argv[]) {
  if (argc < 6) {
    std::cerr << "usage: --create-character <username> <name> <type> <color> "
                 "--database-config <path>\n";
    return 2;
  }
  const std::string_view username(argv[2]);
  const std::string_view name(argv[3]);
  unsigned int character_type = 0;
  unsigned int color_index = 0;
  const std::string_view type_text(argv[4]);
  const std::string_view color_text(argv[5]);
  const auto type_result =
      std::from_chars(type_text.data(), type_text.data() + type_text.size(),
                      character_type);
  const auto color_result =
      std::from_chars(color_text.data(), color_text.data() + color_text.size(),
                      color_index);
  if (type_result.ec != std::errc{} || color_result.ec != std::errc{}) {
    std::cerr << "Character type and color must be decimal numbers.\n";
    return 2;
  }
  std::filesystem::path configuration;
  for (int index = 6; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--database-config" && configuration.empty() &&
        index + 1 < argc) {
      configuration = argv[++index];
    } else {
      std::cerr << "Invalid create-character arguments.\n";
      return 2;
    }
  }
  if (configuration.empty()) {
    std::cerr << "--database-config is required.\n";
    return 2;
  }
  const int units = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()),
      nullptr, 0);
  if (units <= 0) {
    std::cerr << "Character name must be UTF-8.\n";
    return 2;
  }
  std::wstring wide(static_cast<std::size_t>(units), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                      static_cast<int>(name.size()), wide.data(), units);
  protocol::CreateUserInfoRequest request;
  request.character_name.assign(wide.begin(), wide.end());
  request.character_type = character_type;
  request.color_index = color_index;
  try {
    const auto normalized =
        features::authentication::AccountAuthentication::NormalizeUsername(
            username);
    persistence::MySqlAccountStore accounts(configuration);
    const auto account = accounts.FindAccount(normalized);
    if (!account || account->account_id == 0) {
      std::cerr << "Account not found.\n";
      return 3;
    }
    if (account->is_disabled) {
      std::cerr << "Account is disabled.\n";
      return 3;
    }
    persistence::MySqlCharacterStore characters(configuration);
    features::characters::CharacterCreation creation(characters);
    const auto result = creation.Create(account->account_id, request);
    if (result != 0) {
      std::cerr << "Character creation refused with code " << result << ".\n";
      return 4;
    }
  } catch (const std::exception& error) {
    std::cerr << "Character error: " << error.what() << '\n';
    return 1;
  }
  std::cout << "Character created for account " << username << ".\n";
  return 0;
}
}  // namespace

namespace {
// Local administration: replace one account's password without recreating it
// (characters, inventory and tickets stay). Same 6..18 UTF-8 byte rule the
// registration path enforces.
int RunSetPassword(int argc, char* argv[]) {
  if (argc < 4) {
    std::cerr << "usage: --set-password <username> <new-password> "
                 "--database-config <path>\n";
    return 2;
  }
  const std::string_view username(argv[2]);
  const std::string_view password(argv[3]);
  if (password.size() < 6 || password.size() > 18 ||
      password.find('\0') != std::string_view::npos) {
    std::cerr << "Password must be 6 to 18 UTF-8 bytes.\n";
    return 2;
  }
  std::filesystem::path configuration;
  for (int index = 4; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--database-config" && configuration.empty() &&
        index + 1 < argc) {
      configuration = argv[++index];
    } else {
      std::cerr << "Invalid set-password arguments.\n";
      return 2;
    }
  }
  if (configuration.empty()) {
    std::cerr << "--database-config is required.\n";
    return 2;
  }
  try {
    const auto normalized =
        features::authentication::AccountAuthentication::NormalizeUsername(
            username);
    const auto verifier =
        features::authentication::CredentialCrypto::HashPassword(password);
    persistence::MySqlAccountStore accounts(configuration);
    if (!accounts.SetPassword(normalized, verifier)) {
      std::cerr << "Account not found or disabled.\n";
      return 3;
    }
  } catch (const std::exception& error) {
    std::cerr << "Password error: " << error.what() << '\n';
    return 1;
  }
  std::cout << "Password updated for account " << username << ".\n";
  return 0;
}
}  // namespace

namespace {
// Local administration: set one account's balances (金币 = Lucci,
// 泡泡点 = Cash) without touching anything else in the character row.
int RunSetCurrency(int argc, char* argv[]) {
  if (argc < 5) {
    std::cerr << "usage: --set-currency <username> <lucci> <cash> "
                 "--database-config <path>\n";
    return 2;
  }
  const std::string_view username(argv[2]);
  unsigned int lucci = 0;
  unsigned int cash = 0;
  const std::string_view lucci_text(argv[3]);
  const std::string_view cash_text(argv[4]);
  const auto lucci_result = std::from_chars(
      lucci_text.data(), lucci_text.data() + lucci_text.size(), lucci);
  const auto cash_result =
      std::from_chars(cash_text.data(), cash_text.data() + cash_text.size(), cash);
  if (lucci_result.ec != std::errc{} || cash_result.ec != std::errc{}) {
    std::cerr << "Balances must be decimal numbers.\n";
    return 2;
  }
  std::filesystem::path configuration;
  for (int index = 5; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--database-config" && configuration.empty() &&
        index + 1 < argc) {
      configuration = argv[++index];
    } else {
      std::cerr << "Invalid set-currency arguments.\n";
      return 2;
    }
  }
  if (configuration.empty()) {
    std::cerr << "--database-config is required.\n";
    return 2;
  }
  try {
    const auto normalized =
        features::authentication::AccountAuthentication::NormalizeUsername(
            username);
    persistence::MySqlAccountStore accounts(configuration);
    const auto account = accounts.FindAccount(normalized);
    if (!account || account->account_id == 0) {
      std::cerr << "Account not found.\n";
      return 3;
    }
    persistence::MySqlCharacterStore characters(configuration);
    // A repeated write returns "0 rows changed", so existence is decided by the
    // read, not by the update's affected-row count.
    const auto before = characters.FindByAccount(account->account_id);
    if (!before) {
      std::cerr << "Account has no character.\n";
      return 3;
    }
    characters.SetCurrency(account->account_id, lucci, cash);
    const auto after = characters.FindByAccount(account->account_id);
    if (!after) {
      std::cerr << "Character read-back failed.\n";
      return 1;
    }
    std::cout << "Balances for " << username << ": lucci "
              << before->statistics.lucci << " -> " << after->statistics.lucci
              << ", cash " << before->statistics.cash << " -> "
              << after->statistics.cash << ".\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Currency error: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace

bool AccountCommand::IsRequested(int argc, char* argv[]) {
  return argc > 1 && (std::string_view(argv[1]) == "--create-account" ||
                      std::string_view(argv[1]) == "--login" ||
                      std::string_view(argv[1]) == "--create-character" ||
                      std::string_view(argv[1]) == "--set-password" ||
                      std::string_view(argv[1]) == "--set-currency");
}

int AccountCommand::Run(int argc, char* argv[]) const {
  if (std::string_view(argv[1]) == "--create-character") {
    return RunCreateCharacter(argc, argv);
  }
  if (std::string_view(argv[1]) == "--set-password") {
    return RunSetPassword(argc, argv);
  }
  if (std::string_view(argv[1]) == "--set-currency") {
    return RunSetCurrency(argc, argv);
  }
  const bool create = std::string_view(argv[1]) == "--create-account";
  std::filesystem::path configuration;
  std::filesystem::path ticket_path;
  if (argc < 3) {
    std::cerr << "A username is required.\n";
    return 2;
  }
  for (int index = 3; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--database-config" && configuration.empty() &&
        index + 1 < argc) {
      configuration = argv[++index];
    } else if (argument == "--ticket-file" && !create && ticket_path.empty() &&
               index + 1 < argc) {
      ticket_path = argv[++index];
    } else {
      std::cerr << "Invalid account command arguments.\n";
      return 2;
    }
  }
  if (configuration.empty() || (!create && ticket_path.empty())) {
    std::cerr
        << "--database-config and, for login, --ticket-file are required.\n";
    return 2;
  }
  try {
    const auto username =
        features::authentication::AccountAuthentication::NormalizeUsername(
            argv[2]);
    persistence::MySqlAccountStore store(configuration);
    features::authentication::AccountAuthentication authentication(store);
    const PasswordInput password;
    if (create) {
      if (!authentication.CreateAccount(username, password.Value())) {
        std::cerr << "Account already exists.\n";
        return 3;
      }
      std::cout << "Account created.\n";
    } else {
      auto ticket = authentication.Login(username, password.Value());
      if (!ticket) {
        std::cerr << "Authentication failed.\n";
        return 3;
      }
      try {
        WritePrivateTicket(ticket_path, *ticket);
      } catch (...) {
        SecureZeroMemory(ticket->data(), ticket->size());
        throw;
      }
      SecureZeroMemory(ticket->data(), ticket->size());
      std::cout
          << "Login succeeded; a one-use ticket was written to the requested private file.\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Account error: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace bubble_fighter::server::application
