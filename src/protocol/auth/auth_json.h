#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_AUTH_AUTH_JSON_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_AUTH_AUTH_JSON_H_

#include <json.hpp>
#include <stdexcept>
#include <string_view>

namespace bubble_fighter::server::protocol::auth {
inline nlohmann::json ParseBoundedJson(std::string_view text) {
  if (text.size() > 262144) throw std::invalid_argument("JSON message exceeds limit.");
  auto result = nlohmann::json::parse(text, [](int depth, auto, auto&) {
    if (depth > 32) throw std::invalid_argument("JSON nesting exceeds limit.");
    return true;
  });
  if (!result.is_object()) throw std::invalid_argument("Expected a JSON object.");
  return result;
}
}  // namespace bubble_fighter::server::protocol::auth

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_AUTH_AUTH_JSON_H_
