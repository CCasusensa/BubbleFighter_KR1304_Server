#ifndef BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_LAUNCHER_POLICY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_LAUNCHER_POLICY_H_

#include <cstdint>
#include <filesystem>
#include <vector>

namespace bubble_fighter::server::features::authentication {
// Operator-configured build-a-room tables, handed to launchers through the
// authentication channel (action kPolicy). Sorted, duplicate-free; an empty
// table tells the launcher to leave the feature off.
struct LauncherPolicy {
  std::vector<std::uint16_t> modes;
  std::vector<std::uint16_t> crazy_modes;
  // Ini text: `modes=1,3,8` / `crazy_modes=1,8`; comments start with # or ;.
  // Unknown keys and malformed values are rejected instead of being ignored.
  static LauncherPolicy Load(const std::filesystem::path& path);
};
}  // namespace bubble_fighter::server::features::authentication

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_LAUNCHER_POLICY_H_
