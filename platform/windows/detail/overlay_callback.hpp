/// @file
/// @brief Exception-contained overlay message dispatch and final native detachment.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_OVERLAY_CALLBACK_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_OVERLAY_CALLBACK_HPP

#include <windows.h>

namespace avb::platform::windows::detail {

  /// @brief Dispatch a prepared native message without letting exceptions escape.
  ///
  /// Operations and message-owned pointers are borrowed for this synchronous
  /// invocation only. Failed preparation or an unhandled commit delegates to the
  /// saved procedure. Final destruction forwards first, keeping the old procedure
  /// and context usable during its cleanup, then clears callback reachability.
  /// Exception containment preserves the native boundary, not successful handling.
  template <class Operations>
  [[nodiscard]] LRESULT
  dispatch_overlay_callback(Operations &operations, const UINT message, const WPARAM word_parameter, const LPARAM long_parameter) noexcept
  {
    if (message == WM_NCDESTROY) {
      LRESULT result{};
      try {
        result = operations.forward(message, word_parameter, long_parameter);
      } catch (...) {
      }
      try {
        operations.clear_binding();
      } catch (...) {
      }
      return result;
    }

    try {
      const auto prepared =
        operations.prepare(message, word_parameter, long_parameter);
      if (prepared) {
        const auto forward_after_commit =
          operations.forward_after_commit(*prepared);
        const auto result = operations.commit(*prepared);
        if (result && !forward_after_commit) {
          return *result;
        }
      }
    } catch (...) {
    }

    try {
      return operations.forward(message, word_parameter, long_parameter);
    } catch (...) {
      return 0;
    }
  }

}    // namespace avb::platform::windows::detail

#endif    // AVB0795_PLATFORM_WINDOWS_DETAIL_OVERLAY_CALLBACK_HPP
