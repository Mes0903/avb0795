/// @file
/// @brief XDG preference location and process-lifetime exclusive writer ownership.
#ifndef AVB0795_LINUX_PREFERENCES_HPP
#define AVB0795_LINUX_PREFERENCES_HPP

#include "avb/core/types.hpp"
#include <expected>
#include <filesystem>

namespace avb::linux_platform {
  /// @brief Own the config path and a retained, exclusive writer lock.
  /// Create before any load/save and retain through all application persistence.
  /// The dedicated lock inode remains on disk when released, so rename of the
  /// config cannot invalidate coordination. Separate owners, including in one
  /// process, are rejected without waiting. Moving transfers the lock exactly
  /// once; assignment releases the previous lock and requires its saves to end.
  /// Calls and destruction must not race persistence using this owner.
  /// This owner sets its directory/lock modes but does not change global umask;
  /// composition must establish 0077 before services and every save.
  class LinuxPreferences final {
  public:
    /// @brief Snapshot absolute XDG_CONFIG_HOME or fall back to HOME/.config.
    /// Create the private avb0795 directory (0700) and persistent lock (0600).
    /// Missing usable home, filesystem failures and a busy writer return
    /// ConfigWriteFailure, with native detail where available; no config is loaded.
    [[nodiscard]] static std::expected<LinuxPreferences, core::AppError> create();
    ~LinuxPreferences();
    LinuxPreferences(const LinuxPreferences &) = delete;
    LinuxPreferences &operator=(const LinuxPreferences &) = delete;
    LinuxPreferences(LinuxPreferences &&) noexcept;
    LinuxPreferences &operator=(LinuxPreferences &&) noexcept;
    /// Borrow the selected path until this owner is moved from or destroyed.
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    LinuxPreferences(std::filesystem::path path, int lock) noexcept;
    std::filesystem::path path_;
    int lock_{-1};
  };
}    // namespace avb::linux_platform

#endif
