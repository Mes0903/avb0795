/// @file
/// @brief Startup-only atlas configuration over caller-owned font bytes.
/// Native file discovery/loading stays outside this helper. Registering a font
/// here neither bakes the atlas nor submits a graphics upload.
#ifndef AVB0795_DESKTOP_CONTROLLER_FONT_HPP
#define AVB0795_DESKTOP_CONTROLLER_FONT_HPP

#include "avb/core/types.hpp"

#include <imgui.h>

#include <cstddef>
#include <expected>
#include <limits>
#include <span>

namespace avb::desktop {

  /// @brief Register fixed Latin/CJK coverage without transferring font-byte ownership.
  ///
  /// The atlas borrows data because the host already owns its storage. Keep the
  /// bytes valid and unchanged until the atlas is destroyed; keeping them only
  /// until this function returns would leave deferred font processing dangling.
  /// One fixed range set serves every supported UI language, avoiding atlas
  /// replacement and new texture-upload lifetimes during a language switch.
  ///
  /// Empty/unrepresentable storage and failed font registration return a typed
  /// startup failure. Success means registration only: glyphs still depend on the
  /// supplied font, and baking/upload belongs to the host's fenced startup path.
  /// @pre The atlas is being configured during startup, not concurrently in use.
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

}    // namespace avb::desktop

#endif    // AVB0795_DESKTOP_CONTROLLER_FONT_HPP
