#include "operations/operation_sql.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <stdexcept>

#include "features/authentication/credential_crypto.h"
#include "features/characters/character_creation.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::operations {
namespace {
struct Prepared {
  MYSQL_STMT* statement;
  std::vector<std::string> values;
  std::vector<std::uint64_t> unsigned_values;
  std::vector<std::int64_t> signed_values;
  std::vector<MYSQL_BIND> bindings;
  Prepared(MYSQL* db, std::string_view sql, const std::vector<Json>& input)
      : statement(mysql_stmt_init(db)), values(input.size()),
        unsigned_values(input.size()), signed_values(input.size()), bindings(input.size()) {
    if (!statement) throw persistence::MySqlError(mysql_errno(db));
    if (mysql_stmt_prepare(statement, sql.data(),
                           static_cast<unsigned long>(sql.size())) ||
        mysql_stmt_param_count(statement) != input.size()) {
      const persistence::MySqlError error(mysql_stmt_errno(statement),
                                           mysql_stmt_error(statement));
      mysql_stmt_close(statement);
      throw error;
    }
    for (std::size_t i = 0; i < input.size(); ++i) {
      if (input[i].is_null()) {
        bindings[i].buffer_type = MYSQL_TYPE_NULL;
      } else if (input[i].is_boolean() || input[i].is_number_unsigned()) {
        unsigned_values[i] = input[i].is_boolean() ? input[i].get<bool>() : input[i].get<std::uint64_t>();
        bindings[i] = persistence::my_sql_commands::Number(unsigned_values[i]);
      } else if (input[i].is_number_integer()) {
        signed_values[i] = input[i].get<std::int64_t>();
        bindings[i] = persistence::my_sql_commands::Number(signed_values[i]);
        bindings[i].is_unsigned = false;
      } else if (input[i].is_binary()) {
        const auto& bytes = input[i].get_binary();
        values[i].assign(bytes.begin(), bytes.end());
        bindings[i] = persistence::my_sql_commands::Bytes(
            values[i].data(), static_cast<unsigned long>(values[i].size()), MYSQL_TYPE_BLOB);
      } else {
        values[i] = input[i].is_string() ? input[i].get<std::string>() : input[i].dump();
        bindings[i] = persistence::my_sql_commands::Bytes(
            values[i].data(), static_cast<unsigned long>(values[i].size()),
            MYSQL_TYPE_STRING);
      }
    }
    if ((!bindings.empty() && mysql_stmt_bind_param(statement, bindings.data())) ||
        mysql_stmt_execute(statement)) {
      const auto error = mysql_stmt_errno(statement);
      mysql_stmt_close(statement);
      throw persistence::MySqlError(error);
    }
  }
  ~Prepared() { mysql_stmt_close(statement); }
};
}  // namespace

Json SqlBinary(std::string_view bytes) {
  return Json::binary(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

Json Query(MYSQL* db, std::string_view sql, const std::vector<Json>& values) {
  Prepared query(db, sql, values);
  std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> metadata(
      mysql_stmt_result_metadata(query.statement), mysql_free_result);
  if (!metadata) throw std::runtime_error("Missing SQL result metadata.");
  const auto count = mysql_num_fields(metadata.get());
  const auto* fields = mysql_fetch_fields(metadata.get());
  std::vector<std::array<char, 256>> buffers(count);
  std::vector<MYSQL_BIND> bindings(count);
  std::vector<unsigned long> lengths(count);
  std::unique_ptr<bool[]> nulls(new bool[count]{});
  for (unsigned int i = 0; i < count; ++i) {
    bindings[i] = persistence::my_sql_commands::Bytes(
        buffers[i].data(), 256, MYSQL_TYPE_STRING);
    bindings[i].length = &lengths[i];
    bindings[i].is_null = &nulls[i];
  }
  if (mysql_stmt_store_result(query.statement) ||
      mysql_stmt_bind_result(query.statement, bindings.data()))
    throw persistence::MySqlError(mysql_stmt_errno(query.statement));
  Json rows = Json::array();
  for (;;) {
    const auto status = mysql_stmt_fetch(query.statement);
    if (status == MYSQL_NO_DATA) break;
    if (status && status != MYSQL_DATA_TRUNCATED)
      throw persistence::MySqlError(mysql_stmt_errno(query.statement));
    Json row = Json::object();
    for (unsigned int i = 0; i < count; ++i) {
      const std::string key(fields[i].name);
      if (nulls[i]) { row[key] = nullptr; continue; }
      if (lengths[i] > 512 * 1024) throw std::runtime_error("SQL value exceeds limit.");
      std::string value(lengths[i], '\0');
      auto binding = persistence::my_sql_commands::Bytes(
          value.data(), lengths[i], MYSQL_TYPE_STRING);
      if (mysql_stmt_fetch_column(query.statement, &binding, i, 0))
        throw persistence::MySqlError(mysql_stmt_errno(query.statement));
      const auto type = fields[i].type;
      if (type == MYSQL_TYPE_LONG || type == MYSQL_TYPE_LONGLONG ||
          type == MYSQL_TYPE_TINY || type == MYSQL_TYPE_SHORT ||
          type == MYSQL_TYPE_INT24) {
        row[key] = std::stoull(value);
      } else {
        row[key] = std::move(value);
      }
    }
    rows.push_back(std::move(row));
    if (rows.size() > 10000) throw std::runtime_error("SQL result exceeds limit.");
  }
  return rows;
}
std::uint64_t Execute(MYSQL* db, std::string_view sql, const std::vector<Json>& values) {
  Prepared query(db, sql, values);
  return mysql_stmt_affected_rows(query.statement);
}
std::uint64_t Now() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}
std::string Hex(std::string_view bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const unsigned char byte : bytes) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 15]);
  }
  return result;
}
std::string Unhex(std::string_view text) {
  if (text.size() % 2) throw std::invalid_argument("Invalid hex data.");
  const auto digit = [](char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    throw std::invalid_argument("Invalid hex data.");
  };
  std::string result;
  for (std::size_t i = 0; i < text.size(); i += 2)
    result.push_back(static_cast<char>((digit(text[i]) << 4) | digit(text[i + 1])));
  return result;
}
std::u16string Utf16(std::string_view text) {
  if (text.empty()) return {};
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (!size) throw std::invalid_argument("Invalid UTF-8 text.");
  std::wstring wide(size, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), wide.data(), size);
  return {wide.begin(), wide.end()};
}
std::string Utf8(std::u16string_view text) {
  if (text.empty()) return {};
  const std::wstring wide(text.begin(), text.end());
  const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
      static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (!size) throw std::invalid_argument("Invalid UTF-16 text.");
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
      static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
  return result;
}
std::string NameBytes(std::string_view text) {
  const auto name = Utf16(text);
  if (features::characters::CharacterCreation::ValidateName(name))
    throw std::invalid_argument("Invalid KR character name.");
  return {reinterpret_cast<const char*>(name.data()), name.size() * 2};
}
std::string NameText(std::string_view hex) {
  const auto bytes = Unhex(hex);
  if (bytes.size() % 2) throw std::invalid_argument("Invalid stored name.");
  std::u16string name;
  for (std::size_t i = 0; i < bytes.size(); i += 2)
    name.push_back(static_cast<char16_t>(static_cast<unsigned char>(bytes[i]) |
        (static_cast<unsigned char>(bytes[i + 1]) << 8)));
  return Utf8(name);
}
std::string Digest(std::string_view bytes) {
  const auto digest = features::authentication::CredentialCrypto::Sha256(
      {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
  return {reinterpret_cast<const char*>(digest.data()), digest.size()};
}
Json ReadJson(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::invalid_argument("Cannot read operations configuration.");
  return Json::parse(stream);
}
}  // namespace bubble_fighter::server::operations
