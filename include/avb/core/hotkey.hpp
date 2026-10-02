/// @file
/// @brief Platform-independent typed hotkey parsing and canonical text conversion.
#ifndef AVB0795_CORE_HOTKEY_HPP
#define AVB0795_CORE_HOTKEY_HPP

#include "avb/core/types.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace avb::core {

  /// @brief Parse an ASCII chord into an independent value without retaining text.
  /// Accept case-insensitive '+' tokens for Ctrl/Alt/Shift/Meta and one A-Z, 0-9,
  /// or F1-F12 key in any order; modifiers are optional. Reject duplicate modifiers,
  /// extra keys and empty tokens with InvalidValue. Whitespace is not trimmed and
  /// OS availability is not checked. Numeric function-key spelling may have a
  /// leading zero, which serialization removes.
  [[nodiscard]] std::expected<HotkeyChord, AppError>
  parse_hotkey_chord(std::string_view text) noexcept;

  /// @brief Produce owned canonical text in Ctrl, Alt, Shift, Meta, key order.
  /// @pre chord.key is a supported enumerator; this formatter is not a validator
  /// for enum values fabricated outside the parser.
  [[nodiscard]] std::string serialize_hotkey_chord(const HotkeyChord &chord);

  /// @brief Derive owned display text from the same spelling used for persistence.
  /// The result describes a desired chord, not proof of successful OS registration.
  [[nodiscard]] std::string hotkey_display_text(const HotkeyChord &chord);

}    // namespace avb::core

#endif    // AVB0795_CORE_HOTKEY_HPP
