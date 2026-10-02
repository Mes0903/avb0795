/// @file
/// @brief Linux environment snapshot mapped to the existing UI language policy.
#ifndef AVB0795_LINUX_UI_LANGUAGE_HPP
#define AVB0795_LINUX_UI_LANGUAGE_HPP

#include "avb/i18n/language.hpp"

namespace avb::linux_platform {
  /// @brief Resolve a snapshot without mutating process locale or retaining strings.
  /// First nonempty LC_ALL, LC_MESSAGES, LANG is effective. C/POSIX, including
  /// codeset forms, forces English; otherwise try ordered LANGUAGE then effective
  /// locale, stripping codeset/modifier and normalizing underscores privately.
  /// Unsupported/malformed tags fall back to English through the shared matcher.
  /// Caller must coordinate any concurrent environment mutation.
  [[nodiscard]] i18n::Language linux_ui_language();
}    // namespace avb::linux_platform

#endif
