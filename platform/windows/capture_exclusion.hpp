#ifndef AIRELINNA_PLATFORM_WINDOWS_CAPTURE_EXCLUSION_HPP
#define AIRELINNA_PLATFORM_WINDOWS_CAPTURE_EXCLUSION_HPP

#include "airelinna/core/types.hpp"

#include <windows.h>

namespace airelinna::platform::windows {

  // Applies the strongest capture exclusion supported by the current Windows
  // compositor. Excluded means Windows accepted the requested affinity; it is
  // not a guarantee about every third-party capture mechanism.
  [[nodiscard]] core::CaptureExclusionStatus
  attempt_capture_exclusion(HWND window) noexcept;

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_CAPTURE_EXCLUSION_HPP
