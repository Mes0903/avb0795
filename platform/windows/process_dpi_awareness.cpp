/// @file
/// @brief Early process/thread DPI-awareness verification before creating windows.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/process_dpi_awareness.hpp"

#include <windows.h>

#include <cstdint>

namespace avb::platform::windows {

  std::expected<void, core::AppError>
  establish_per_monitor_v2_dpi_awareness() noexcept
  {
    SetLastError(ERROR_SUCCESS);
    const auto established =
      SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto establish_error = established != FALSE ? ERROR_SUCCESS
                                                      : GetLastError();

    // A manifest can make the setter fail legitimately; verify effective awareness instead.
    const auto current = GetThreadDpiAwarenessContext();
    if (current != nullptr &&
        AreDpiAwarenessContextsEqual(
          current, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
      return {};
    }

    return std::unexpected{core::AppError{
      core::AppErrorCode::RuntimeConstructionFailure,
      static_cast<std::int64_t>(establish_error)}};
  }

}    // namespace avb::platform::windows
