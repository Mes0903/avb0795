/// @file
/// @brief Windows preferred-UI-language lookup with a deterministic fallback.
#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_UI_LANGUAGE_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_UI_LANGUAGE_HPP

#include "avb/i18n/language.hpp"

namespace avb::platform::windows {

  /// @brief Select the first supported language in Windows user preference order.
  ///
  /// The result is a value snapshot, not a subscription to later system changes.
  /// Unsupported preferences and query, format, or allocation failures fall back
  /// to English. Explicit saved language selection remains the caller's policy.
  [[nodiscard]] i18n::Language windows_ui_language() noexcept;

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_UI_LANGUAGE_HPP
