/// @file Platform-independent typed hotkey parsing and canonical text conversion.
#ifndef AIRELINNA_CORE_HOTKEY_HPP
#define AIRELINNA_CORE_HOTKEY_HPP

#include "airelinna/core/types.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace airelinna::core {

  /// Parse case-insensitive '+' tokens for Ctrl/Alt/Shift/Meta and one A-Z, 0-9,
  /// or F1-F12 key. Reject duplicate modifiers, extra keys and empty tokens with
  /// InvalidValue; whitespace is not trimmed and OS availability is not checked.
  [[nodiscard]] std::expected<HotkeyChord, AppError>
  parse_hotkey_chord(std::string_view text) noexcept;

  /// Serialize a supported typed chord in Ctrl, Alt, Shift, Meta, key order.
  [[nodiscard]] std::string serialize_hotkey_chord(const HotkeyChord &chord);

  /// Derive display text from the same spelling used for persistence.
  [[nodiscard]] std::string hotkey_display_text(const HotkeyChord &chord);

}    // namespace airelinna::core

#endif    // AIRELINNA_CORE_HOTKEY_HPP
