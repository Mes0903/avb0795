/// @file
/// @brief Versioned preference persistence with one injected atomic-replace operation.
#ifndef AVB0795_APP_CONFIG_STORE_HPP
#define AVB0795_APP_CONFIG_STORE_HPP

#include "avb/core/types.hpp"
#include "avb/i18n/language.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>

namespace avb::app {

  /// @brief Persisted V1 preferences, initialized to the complete first-run defaults.
  /// Language stores the user's request, including System, so saving never freezes
  /// a resolved system language. Mode is the user's accepted preference, independent
  /// of temporary region-selection overrides. Source/title/pixels/geometry/crop and
  /// editing drafts are excluded; this value does not validate arbitrary enum casts.
  struct UserConfig {
    std::uint32_t version{1};
    core::HotkeyChord hotkey{core::HotkeyModifiers{.control = true, .alt = true},
                             core::HotkeyKey::R};
    core::FpsCap fps_cap{core::FpsCap::Fps60};
    core::BorderPreference border{core::BorderPreference::Bordered};
    core::ExtensionSettings extensions{};
    i18n::LanguagePreference language{i18n::LanguagePreference::System};
    int opacity_percent{100};
    core::OverlayMode mode{core::OverlayMode::Editable};
    bool auto_refresh_enabled{};

    friend bool operator==(const UserConfig &, const UserConfig &) = default;
  };

  /// @brief Commit a closed sibling temporary file using platform replacement policy.
  /// Borrow both paths only for the call. Success must make the temporary content
  /// the destination; failure should preserve the old destination. Atomic visibility
  /// and native durability guarantees belong to this injected operation, while
  /// ConfigStore attempts failed-temporary cleanup. Do not reenter saves to the same path.
  using AtomicReplaceFn =
    std::function<std::expected<void, core::AppError>(
      const std::filesystem::path &temporary,
      const std::filesystem::path &destination)>;

  /// @brief Read and replace JSON preferences synchronously without caching accepted state.
  /// Application owns when to save and its dirty bit. A deterministic '.tmp' sibling
  /// is used, so callers must coordinate concurrent writers, including separate store
  /// instances. Const methods do not imply synchronized I/O or a thread-safe callable.
  class ConfigStore final {
  public:
    /// Own the replacement callable for all subsequent saves.
    explicit ConfigStore(AtomicReplaceFn atomic_replace);

    /// @brief Load an owned config value without rewriting or repairing the file.
    /// Missing files and invalid JSON/schema/version/required values return complete
    /// defaults; a missing extensions field uses disabled extensions, but a malformed
    /// one invalidates the whole config. Missing or invalid language alone recovers
    /// locally to System; missing or invalid opacity alone recovers to 100.
    /// Missing or invalid mode and auto_refresh_enabled independently recover to
    /// Editable and false. A nonempty borrowed legacy_path is read only when path
    /// is confirmed missing; an existing malformed/unreadable primary never falls
    /// back. Existence-check errors return ConfigReadFailure. Neither file is written.
    /// Read failures or detectable size changes return
    /// ConfigReadFailure. This is not a synchronized snapshot of concurrent writers;
    /// allocation/path exceptions are not all translated into AppError.
    [[nodiscard]] std::expected<UserConfig, core::AppError>
    load(const std::filesystem::path &path,
         const std::filesystem::path &legacy_path = {}) const;

    /// @brief Serialize the config, then write/flush/close a sibling before committing it.
    /// Unsupported version, language, mode, or opacity outside 0..100 returns
    /// InvalidValue before I/O. The destination is always the explicit path, even
    /// when the caller obtained config from a legacy fallback.
    /// Other typed fields must contain supported values; save is not a general enum
    /// validator. Handled serialization/write/replace failures return ConfigWriteFailure,
    /// retain replacement-native detail where available, and attempt temporary cleanup.
    /// Path and stream setup may still throw. Parent directories are not created;
    /// atomic replacement and crash durability depend on the injected callable.
    [[nodiscard]] std::expected<void, core::AppError>
    save(const std::filesystem::path &path, const UserConfig &config) const;

  private:
    AtomicReplaceFn atomic_replace_;
  };

}    // namespace avb::app

#endif    // AVB0795_APP_CONFIG_STORE_HPP
