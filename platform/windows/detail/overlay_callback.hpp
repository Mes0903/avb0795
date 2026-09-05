#ifndef AIRELINNA_PLATFORM_WINDOWS_DETAIL_OVERLAY_CALLBACK_HPP
#define AIRELINNA_PLATFORM_WINDOWS_DETAIL_OVERLAY_CALLBACK_HPP

#include <windows.h>

namespace airelinna::platform::windows::detail {

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

}    // namespace airelinna::platform::windows::detail

#endif    // AIRELINNA_PLATFORM_WINDOWS_DETAIL_OVERLAY_CALLBACK_HPP
