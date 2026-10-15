#include "features/authentication/launcher_policy.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bubble_fighter::server::features::authentication {
namespace {
std::vector<std::uint16_t> ParseModes(std::string_view key,
                                      std::string_view value) {
  std::vector<std::uint16_t> modes;
  std::string token;
  const auto flush = [&] {
    if (token.empty()) return;
    unsigned long parsed = 0;
    try {
      parsed = std::stoul(token);
    } catch (...) {
      throw std::invalid_argument("Invalid launcher policy mode in " +
                                  std::string(key) + ".");
    }
    if (parsed > 65535)
      throw std::invalid_argument("Launcher policy mode out of range in " +
                                  std::string(key) + ".");
    const auto mode = static_cast<std::uint16_t>(parsed);
    if (std::find(modes.begin(), modes.end(), mode) == modes.end())
      modes.push_back(mode);
    token.clear();
  };
  for (const auto value_character : value) {
    if (value_character == ',' || value_character == ' ' ||
        value_character == '\t') {
      flush();
    } else if (value_character >= '0' && value_character <= '9') {
      token.push_back(value_character);
    } else {
      throw std::invalid_argument("Invalid character in launcher policy " +
                                  std::string(key) + ".");
    }
  }
  flush();
  std::sort(modes.begin(), modes.end());
  return modes;
}
}  // namespace

LauncherPolicy LauncherPolicy::Load(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Unable to read the launcher policy file.");
  LauncherPolicy policy;
  bool modes_seen = false;
  bool crazy_seen = false;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#' ||
        line[first] == ';' || line[first] == '[') {
      continue;
    }
    const auto separator = line.find('=', first);
    if (separator == std::string::npos) {
      throw std::invalid_argument("Launcher policy entries must be key=value.");
    }
    std::string key = line.substr(first, separator - first);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
      key.pop_back();
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char value) {
      return static_cast<char>(std::tolower(value));
    });
    const auto value = line.substr(separator + 1);
    if (key == "modes") {
      if (modes_seen)
        throw std::invalid_argument("Duplicate launcher policy entry: modes.");
      modes_seen = true;
      policy.modes = ParseModes(key, value);
    } else if (key == "crazy_modes") {
      if (crazy_seen)
        throw std::invalid_argument(
            "Duplicate launcher policy entry: crazy_modes.");
      crazy_seen = true;
      policy.crazy_modes = ParseModes(key, value);
    } else {
      throw std::invalid_argument("Unsupported launcher policy entry: " + key +
                                  ".");
    }
  }
  return policy;
}
}  // namespace bubble_fighter::server::features::authentication
