/// @file
/// @brief Allocation-free chord validation and canonical owned text formatting.
#include "avb/core/hotkey.hpp"

#include <cstddef>
#include <string_view>

namespace avb::core {
  namespace {

    // Hotkey grammar is ASCII and independent of the process/user locale.
    [[nodiscard]] constexpr char ascii_upper(char character) noexcept
    {
      if (character >= 'a' && character <= 'z') {
        return static_cast<char>(character - ('a' - 'A'));
      }
      return character;
    }

    [[nodiscard]] constexpr bool token_equals(std::string_view token,
                                              std::string_view expected) noexcept
    {
      if (token.size() != expected.size()) {
        return false;
      }

      for (std::size_t index{}; index < token.size(); ++index) {
        if (ascii_upper(token[index]) != ascii_upper(expected[index])) {
          return false;
        }
      }
      return true;
    }

    [[nodiscard]] constexpr std::expected<HotkeyKey, AppError>
    parse_key(std::string_view token) noexcept
    {
      if (token.size() == 1) {
        const auto character = ascii_upper(token.front());
        if (character >= 'A' && character <= 'Z') {
          return static_cast<HotkeyKey>(static_cast<int>(HotkeyKey::A) +
                                        (character - 'A'));
        }
        if (character >= '0' && character <= '9') {
          return static_cast<HotkeyKey>(static_cast<int>(HotkeyKey::Digit0) +
                                        (character - '0'));
        }
      }

      if (token.size() >= 2 && token.size() <= 3 &&
          ascii_upper(token.front()) == 'F') {
        int number{};
        for (const auto character : token.substr(1)) {
          if (character < '0' || character > '9') {
            return std::unexpected{AppError{AppErrorCode::InvalidValue}};
          }
          number = (number * 10) + (character - '0');
        }
        if (number >= 1 && number <= 12) {
          return static_cast<HotkeyKey>(static_cast<int>(HotkeyKey::F1) +
                                        (number - 1));
        }
      }

      return std::unexpected{AppError{AppErrorCode::InvalidValue}};
    }

    [[nodiscard]] constexpr AppError invalid_value() noexcept
    {
      return AppError{AppErrorCode::InvalidValue};
    }

    void append_modifier(std::string &text, bool present, std::string_view spelling)
    {
      if (!present) {
        return;
      }
      if (!text.empty()) {
        text.push_back('+');
      }
      text.append(spelling);
    }

    [[nodiscard]] std::string key_text(HotkeyKey key)
    {
      const auto value = static_cast<int>(key);
      if (value >= static_cast<int>(HotkeyKey::A) &&
          value <= static_cast<int>(HotkeyKey::Z)) {
        return std::string(
          1, static_cast<char>('A' + (value - static_cast<int>(HotkeyKey::A))));
      }
      if (value >= static_cast<int>(HotkeyKey::Digit0) &&
          value <= static_cast<int>(HotkeyKey::Digit9)) {
        return std::string(
          1,
          static_cast<char>('0' + (value - static_cast<int>(HotkeyKey::Digit0))));
      }
      if (value >= static_cast<int>(HotkeyKey::F1) &&
          value <= static_cast<int>(HotkeyKey::F12)) {
        return "F" + std::to_string(value - static_cast<int>(HotkeyKey::F1) + 1);
      }
      return {};
    }

  }    // namespace

  std::expected<HotkeyChord, AppError>
  parse_hotkey_chord(std::string_view text) noexcept
  {
    if (text.empty()) {
      return std::unexpected{invalid_value()};
    }

    HotkeyChord chord{};
    bool has_key{};
    std::size_t token_start{};
    while (token_start <= text.size()) {
      const auto separator = text.find('+', token_start);
      const auto token_end =
        separator == std::string_view::npos ? text.size() : separator;
      const auto token = text.substr(token_start, token_end - token_start);
      if (token.empty()) {
        return std::unexpected{invalid_value()};
      }

      // Point only into the local chord to share duplicate detection across modifiers.
      bool *modifier{};
      if (token_equals(token, "Ctrl")) {
        modifier = &chord.modifiers.control;
      }
      else if (token_equals(token, "Alt")) {
        modifier = &chord.modifiers.alt;
      }
      else if (token_equals(token, "Shift")) {
        modifier = &chord.modifiers.shift;
      }
      else if (token_equals(token, "Meta")) {
        modifier = &chord.modifiers.meta;
      }

      if (modifier != nullptr) {
        if (*modifier) {
          return std::unexpected{invalid_value()};
        }
        *modifier = true;
      }
      else {
        if (has_key) {
          return std::unexpected{invalid_value()};
        }
        const auto parsed_key = parse_key(token);
        if (!parsed_key) {
          return std::unexpected{parsed_key.error()};
        }
        chord.key = *parsed_key;
        has_key = true;
      }

      if (separator == std::string_view::npos) {
        break;
      }
      token_start = separator + 1;
    }

    // Modifiers alone cannot name a recover-to-editable hotkey binding.
    if (!has_key) {
      return std::unexpected{invalid_value()};
    }
    return chord;
  }

  std::string serialize_hotkey_chord(const HotkeyChord &chord)
  {
    std::string text;
    append_modifier(text, chord.modifiers.control, "Ctrl");
    append_modifier(text, chord.modifiers.alt, "Alt");
    append_modifier(text, chord.modifiers.shift, "Shift");
    append_modifier(text, chord.modifiers.meta, "Meta");
    append_modifier(text, true, key_text(chord.key));
    return text;
  }

  std::string hotkey_display_text(const HotkeyChord &chord)
  {
    return serialize_hotkey_chord(chord);
  }

}    // namespace avb::core
