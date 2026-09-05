#ifndef AIRELINNA_CORE_HOTKEY_HPP
#define AIRELINNA_CORE_HOTKEY_HPP

#include "airelinna/core/types.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace airelinna::core {

  [[nodiscard]] std::expected<HotkeyChord, AppError>
  parse_hotkey_chord(std::string_view text) noexcept;

  [[nodiscard]] std::string serialize_hotkey_chord(const HotkeyChord &chord);

  [[nodiscard]] std::string hotkey_display_text(const HotkeyChord &chord);

}    // namespace airelinna::core

#endif    // AIRELINNA_CORE_HOTKEY_HPP
