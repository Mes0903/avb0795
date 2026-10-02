/// @file
/// @brief Explicitly disabled Linux global-shortcut service for the first version.
#ifndef AVB0795_LINUX_DISABLED_HOTKEY_HPP
#define AVB0795_LINUX_DISABLED_HOTKEY_HPP

#include "avb/app/ports.hpp"

namespace avb::linux_platform {
  /// @brief Reject shortcut registration while retaining the user's desired chord elsewhere.
  /// Compose with global_hotkey=false so unavailable registration is explained
  /// before command admission. This synchronous main-thread service owns no
  /// native registration, callback, worker or wake borrower; destruction requires
  /// no retirement. An explicit replace fails without changing any process state.
  class LinuxDisabledHotkey final : public app::IGlobalHotkey {
  public:
    [[nodiscard]] std::expected<void, core::AppError>
    replace(const core::HotkeyChord &) override
    {
      return std::unexpected{core::AppError{core::AppErrorCode::InvalidCommand}};
    }

    void clear() noexcept override {}
  };
}    // namespace avb::linux_platform

#endif
