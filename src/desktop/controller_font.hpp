/// @file
/// Startup-only font-atlas configuration; native font loading is platform-owned.
#ifndef AIRELINNA_DESKTOP_CONTROLLER_FONT_HPP
#define AIRELINNA_DESKTOP_CONTROLLER_FONT_HPP

#include "airelinna/core/types.hpp"

#include <imgui.h>

#include <cstddef>
#include <expected>
#include <limits>
#include <span>

namespace airelinna::desktop {

  /// Register the supplied system font and fixed Latin/CJK glyph coverage.
  /// The caller retains the bytes until the atlas is destroyed. Baking/upload
  /// stays on the host's startup/fence path instead of dynamic backend uploads.
  [[nodiscard]] inline std::expected<void, core::AppError>
  configure_controller_font(ImFontAtlas &atlas, std::span<std::byte> data)
  {
    if (data.empty() || data.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
      return std::unexpected{core::AppError{core::AppErrorCode::RuntimeConstructionFailure}};
    }
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = 1;
    config.OversampleV = 1;
    // Latin, punctuation, kana, bopomofo, BMP CJK (including Extension A),
    // compatibility ideographs and full-width forms supported by the font.
    static constexpr ImWchar ranges[]{
      0x0020, 0x00FF, 0x2000, 0x206F, 0x3000, 0x9FFF,
      0xF900, 0xFAFF, 0xFF00, 0xFFEF, 0};
    atlas.TexMinWidth = 2048;
    if (atlas.AddFontFromMemoryTTF(data.data(), static_cast<int>(data.size()),
                                   18.0F, &config, ranges) == nullptr) {
      return std::unexpected{core::AppError{core::AppErrorCode::RuntimeConstructionFailure}};
    }
    return {};
  }

}    // namespace airelinna::desktop

#endif    // AIRELINNA_DESKTOP_CONTROLLER_FONT_HPP
