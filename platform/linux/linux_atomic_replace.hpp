/// @file
/// @brief Permission enforcement and atomic visibility for closed preference temporaries.
#ifndef AVB0795_LINUX_ATOMIC_REPLACE_HPP
#define AVB0795_LINUX_ATOMIC_REPLACE_HPP

#include "avb/core/types.hpp"
#include <expected>
#include <filesystem>

namespace avb::linux_platform {
  /// @brief Commit a closed regular sibling temporary after mode 0600 and fsync.
  /// Borrow paths for this synchronous call. Caller holds LinuxPreferences' writer
  /// lock and prevents path mutation during the operation. Reject symlink/nonregular
  /// inputs and distinct directories. All checks occur before rename: failure
  /// returns ConfigWriteFailure with native detail and preserves the destination;
  /// the temporary may have its mode changed. Success consumes the temporary.
  /// This gives atomic visibility, not directory durability after power loss.
  [[nodiscard]] std::expected<void, core::AppError> linux_atomic_replace(
    const std::filesystem::path &temporary, const std::filesystem::path &destination);
}    // namespace avb::linux_platform

#endif
