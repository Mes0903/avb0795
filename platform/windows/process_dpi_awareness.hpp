/// @file
/// @brief Early process/thread DPI-awareness verification before creating windows.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_PROCESS_DPI_AWARENESS_HPP
#define AVB0795_PLATFORM_WINDOWS_PROCESS_DPI_AWARENESS_HPP

#include "avb/core/types.hpp"

#include <expected>

namespace avb::platform::windows {

  /// Establish and verify effective PER_MONITOR_AWARE_V2 before either window exists.
  /// An already-effective manifest setting is accepted even if the setter fails.
  [[nodiscard]] std::expected<void, core::AppError>
  establish_per_monitor_v2_dpi_awareness() noexcept;

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_PROCESS_DPI_AWARENESS_HPP
