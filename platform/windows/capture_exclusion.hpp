/// @file
/// @brief Best-effort capture exclusion and validation of the patched GLFW window class.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_CAPTURE_EXCLUSION_HPP
#define AVB0795_PLATFORM_WINDOWS_CAPTURE_EXCLUSION_HPP

#include "avb/core/types.hpp"

#include <windows.h>

#include <expected>

namespace avb::platform::windows {

  /// @brief Request compositor capture exclusion for an owned top-level window.
  ///
  /// The borrowed window must belong to the calling thread and process; identity
  /// is checked before and after the native call. Excluded means Windows accepted
  /// the requested affinity, not that every third-party capture path will honor
  /// it. Unsupported and failed attempts remain reportable status values.
  [[nodiscard]] core::CaptureExclusionStatus
  attempt_capture_exclusion(HWND window) noexcept;

  /// @brief Validate the controller window class while preserving capture visibility.
  ///
  /// Class styles incompatible with layered input policy are startup errors.
  /// A valid, newly created controller retains its default capturable affinity
  /// so screenshots and screen sharing can include its contents. Successful
  /// validation returns NotAttempted through the existing status channel.
  /// Window-source eligibility remains a separate catalog policy based on the
  /// owning process, independent of this controller setup operation.
  [[nodiscard]] std::expected<core::CaptureExclusionStatus, core::AppError>
  prepare_controller_window(HWND window) noexcept;

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_CAPTURE_EXCLUSION_HPP
