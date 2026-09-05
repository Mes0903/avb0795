/// @file
/// @brief Best-effort capture exclusion and validation of the patched GLFW window class.
/// Native implementation details remain below the portable application ports.

#ifndef AIRELINNA_PLATFORM_WINDOWS_CAPTURE_EXCLUSION_HPP
#define AIRELINNA_PLATFORM_WINDOWS_CAPTURE_EXCLUSION_HPP

#include "airelinna/core/types.hpp"

#include <windows.h>

#include <expected>

namespace airelinna::platform::windows {

  // Applies the strongest capture exclusion supported by the current Windows
  // compositor. Excluded means Windows accepted the requested affinity; it is
  // not a guarantee about every third-party capture mechanism.
  /// Request exclusion only for an owned top-level window on the creating thread.
  [[nodiscard]] core::CaptureExclusionStatus
  attempt_capture_exclusion(HWND window) noexcept;

  // Validates the patched GLFW controller class before applying best-effort
  // capture exclusion. A capture-exclusion failure remains a portable status;
  // an invalid class is a startup error because Pass-through would be unsafe.
  /// Validate class compatibility before reporting nonfatal compositor exclusion status.
  [[nodiscard]] std::expected<core::CaptureExclusionStatus, core::AppError>
  prepare_controller_window(HWND window) noexcept;

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_CAPTURE_EXCLUSION_HPP
