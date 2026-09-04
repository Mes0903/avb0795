#ifndef AIRELINNA_APP_CONFIG_STORE_HPP
#define AIRELINNA_APP_CONFIG_STORE_HPP

#include "airelinna/core/types.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>

namespace airelinna::app {

struct UserConfig {
  std::uint32_t version{1};
  core::HotkeyChord hotkey{core::HotkeyModifiers{.control = true, .alt = true},
                           core::HotkeyKey::R};
  core::FpsCap fps_cap{core::FpsCap::Fps60};
  core::BorderPreference border{core::BorderPreference::Bordered};

  friend bool operator==(const UserConfig &, const UserConfig &) = default;
};

using AtomicReplaceFn =
    std::function<bool(const std::filesystem::path &temporary,
                       const std::filesystem::path &destination)>;

class ConfigStore final {
public:
  explicit ConfigStore(AtomicReplaceFn atomic_replace);

  [[nodiscard]] std::expected<UserConfig, core::AppError>
  load(const std::filesystem::path &path) const;

  [[nodiscard]] std::expected<void, core::AppError>
  save(const std::filesystem::path &path, const UserConfig &config) const;

private:
  AtomicReplaceFn atomic_replace_;
};

} // namespace airelinna::app

#endif // AIRELINNA_APP_CONFIG_STORE_HPP
