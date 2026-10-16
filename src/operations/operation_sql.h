#ifndef BUBBLEFIGHTER_SERVER_OPERATIONS_OPERATION_SQL_H_
#define BUBBLEFIGHTER_SERVER_OPERATIONS_OPERATION_SQL_H_

#include <mysql.h>
#include <json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bubble_fighter::server::operations {
using Json = nlohmann::json;
Json SqlBinary(std::string_view bytes);
// Bound values, including binary strings, never become SQL syntax.
Json Query(MYSQL* db, std::string_view sql,
           const std::vector<Json>& values = {});
std::uint64_t Execute(MYSQL* db, std::string_view sql,
                      const std::vector<Json>& values = {});
std::uint64_t Now();
std::string Hex(std::string_view bytes);
std::string Unhex(std::string_view text);
std::u16string Utf16(std::string_view text);
std::string Utf8(std::u16string_view text);
std::string NameBytes(std::string_view text);
std::string NameText(std::string_view hex);
std::string Digest(std::string_view bytes);
Json ReadJson(const std::filesystem::path& path);
}  // namespace bubble_fighter::server::operations

#endif  // BUBBLEFIGHTER_SERVER_OPERATIONS_OPERATION_SQL_H_
