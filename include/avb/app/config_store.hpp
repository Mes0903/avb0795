/// @file Versioned preference persistence with one injected atomic-replace operation.
#ifndef AVB0795_APP_CONFIG_STORE_HPP
#define AVB0795_APP_CONFIG_STORE_HPP

#include "avb/core/types.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>

namespace avb::app {

  /// Persisted V1 preferences only: no source/title/pixels/position or session input mode.
  struct UserConfig {
    std::uint32_t version{1};
    core::HotkeyChord hotkey{core::HotkeyModifiers{.control = true, .alt = true},
                             core::HotkeyKey::R};
    core::FpsCap fps_cap{core::FpsCap::Fps60};
    core::BorderPreference border{core::BorderPreference::Bordered};
    core::ExtensionSettings extensions{};

    friend bool operator==(const UserConfig &, const UserConfig &) = default;
  };

  /// Platform-supplied replacement of a flushed sibling temporary file at destination.
  /// The callable owns native replacement policy; ConfigStore handles failed-temp cleanup.
  using AtomicReplaceFn =
    std::function<std::expected<void, core::AppError>(
      const std::filesystem::path &temporary,
      const std::filesystem::path &destination)>;

  /// Concrete synchronous JSON store; Application owns when to save and its dirty bit.
  /// A deterministic '.tmp' sibling is used, so concurrent writers need external coordination.
  class ConfigStore final {
  public:
    /// Own the replacement callable for all subsequent saves.
    explicit ConfigStore(AtomicReplaceFn atomic_replace);

    /// Return defaults for a missing file or invalid JSON/schema/version/value.
    /// Return ConfigReadFailure for open/read/size inconsistency failures.
    [[nodiscard]] std::expected<UserConfig, core::AppError>
    load(const std::filesystem::path &path) const;

    /// Write/flush/close a temporary file, then invoke atomic replacement.
    /// Unsupported version returns InvalidValue; handled write/replace failures
    /// return ConfigWriteFailure and attempt temporary-file removal.
    [[nodiscard]] std::expected<void, core::AppError>
    save(const std::filesystem::path &path, const UserConfig &config) const;

  private:
    AtomicReplaceFn atomic_replace_;
  };

}    // namespace avb::app

#endif    // AVB0795_APP_CONFIG_STORE_HPP
