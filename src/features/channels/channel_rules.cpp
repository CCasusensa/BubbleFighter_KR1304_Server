#include <charconv>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

#include "features/channels/channel_worlds.h"

namespace bubble_fighter::server::features::channels {
namespace {
std::string Trim(std::string value) {
  const auto start = value.find_first_not_of(" \t\r");
  if (start == std::string::npos) return {};
  return value.substr(start, value.find_last_not_of(" \t\r") - start + 1);
}
std::uint32_t Number(const std::string& text) {
  std::uint32_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::invalid_argument("Invalid channel rule number.");
  return value;
}
}  // namespace
ChannelRules ChannelRules::Read(std::istream& input) {
  ChannelRules rules;
  std::map<std::uint32_t, std::set<std::string>> fields;
  std::uint32_t category = 0;
  std::size_t bytes = 0;
  for (std::string raw; std::getline(input, raw);) {
    bytes += raw.size() + 1;
    if (bytes > 16384)
      throw std::invalid_argument("Channel rules are too large.");
    auto line = Trim(std::move(raw));
    if (line.empty() || line.front() == '#' || line.front() == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      category = Number(line.substr(1, line.size() - 2));
      if ((category != 20 && category != 70 && category != 80) ||
          fields.contains(category))
        throw std::invalid_argument(
            "Unsupported or duplicate channel category.");
      fields.emplace(category, std::set<std::string>{});
      rules.categories_.emplace(category, ChannelLevelRange{});
      continue;
    }
    const auto equals = line.find('=');
    if (!category || equals == std::string::npos)
      throw std::invalid_argument("Channel rule outside a category.");
    const auto key = Trim(line.substr(0, equals));
    const auto value = Number(Trim(line.substr(equals + 1)));
    if (category == 80 && key == "MaximumMercenaries") {
      if (value > 3 || !fields.at(category).insert(key).second)
        throw std::invalid_argument("Unsupported or duplicate channel rule.");
      rules.maximum_mercenaries_ = value;
      continue;
    }
    if ((key != "MinimumLevel" && key != "MaximumLevel") ||
        !fields.at(category).insert(key).second || value < 1 || value > 260)
      throw std::invalid_argument("Unsupported or duplicate channel rule.");
    auto& range = rules.categories_.at(category);
    (key == "MinimumLevel" ? range.minimum : range.maximum) = value;
  }
  if (input.bad()) throw std::runtime_error("Cannot read channel rules.");
  for (const auto& [id, keys] : fields) {
    const auto& range = rules.categories_.at(id);
    if (keys.size() != (id == 80 ? 3U : 2U) || range.minimum > range.maximum)
      throw std::invalid_argument(
          "Incomplete or reversed channel level range.");
  }
  return rules;
}
ChannelRules ChannelRules::Load(const std::filesystem::path& path) {
  std::ifstream input{std::filesystem::path(path)};
  if (!input) throw std::runtime_error("Cannot open channel rules.");
  return Read(input);
}
const std::map<std::uint32_t, ChannelLevelRange>& ChannelRules::Categories()
    const noexcept {
  return categories_;
}
std::uint32_t ChannelRules::MaximumMercenaries() const noexcept {
  return maximum_mercenaries_;
}
}  // namespace bubble_fighter::server::features::channels
