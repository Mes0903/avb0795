/// @file
/// @brief Early process/thread DPI-awareness verification before creating windows.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_PROCESS_DPI_AWARENESS_HPP
#define AVB0795_PLATFORM_WINDOWS_PROCESS_DPI_AWARENESS_HPP

#include "avb/core/types.hpp"

#include <expected>

namespace avb::platform::windows {

  /// @brief Establish and verify effective per-monitor-v2 awareness for window creation.
  ///
  /// A manifest may already have fixed process awareness, so the setter result is
  /// not decisive; the current thread's effective context is checked afterward.
  /// Success establishes the coordinate assumptions used by native window geometry
  /// at this call, not a guarantee against later thread-context overrides.
  /// @pre Call on the window-creating thread before creating any process windows.
  [[nodiscard]] std::expected<void, core::AppError>
  establish_per_monitor_v2_dpi_awareness() noexcept;

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_PROCESS_DPI_AWARENESS_HPP
