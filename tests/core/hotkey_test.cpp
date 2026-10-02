#include "avb/core/hotkey.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

TEST_CASE("avb0795 V1 case 05: typed hotkey validation and serialization",
          "[avb0795-v1]")
{
  using avb::core::AppErrorCode;
  using avb::core::hotkey_display_text;
  using avb::core::HotkeyChord;
  using avb::core::HotkeyKey;
  using avb::core::HotkeyModifiers;
  using avb::core::parse_hotkey_chord;
  using avb::core::serialize_hotkey_chord;

  SECTION("representative chords parse to typed values and canonical text")
  {
    struct Row {
      std::string_view input;
      HotkeyChord chord;
      std::string_view canonical;
    };

    constexpr std::array rows{
      Row{"r", HotkeyChord{{}, HotkeyKey::R}, "R"},
      Row{"shift+ctrl+f12",
          HotkeyChord{HotkeyModifiers{.control = true, .shift = true},
                      HotkeyKey::F12},
          "Ctrl+Shift+F12"},
      Row{"Meta+Alt+7",
          HotkeyChord{HotkeyModifiers{.alt = true, .meta = true},
                      HotkeyKey::Digit7},
          "Alt+Meta+7"},
    };

    for (const auto &row : rows) {
      CAPTURE(row.input);
      const auto parsed = parse_hotkey_chord(row.input);
      REQUIRE(parsed.has_value());
      CHECK(*parsed == row.chord);
      CHECK(serialize_hotkey_chord(*parsed) == row.canonical);
      CHECK(hotkey_display_text(*parsed) == row.canonical);
    }
  }

  SECTION("invalid chord categories are rejected before registration")
  {
    constexpr std::array invalid_chords{
      std::string_view{""},
      std::string_view{"Ctrl+Alt"},
      std::string_view{"Ctrl+ctrl+R"},
      std::string_view{"Ctrl+R+R"},
      std::string_view{"Ctrl+R+F1"},
      std::string_view{"Ctrl+Escape"},
    };

    for (const auto input : invalid_chords) {
      CAPTURE(input);
      const auto parsed = parse_hotkey_chord(input);
      REQUIRE_FALSE(parsed.has_value());
      CHECK(parsed.error().code == AppErrorCode::InvalidValue);
      CHECK_FALSE(parsed.error().retryable());
    }
  }
}
