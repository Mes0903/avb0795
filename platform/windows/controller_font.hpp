/// @file
/// @brief Installed Windows font loading for the independent controller UI.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_CONTROLLER_FONT_HPP
#define AVB0795_PLATFORM_WINDOWS_CONTROLLER_FONT_HPP

#include "avb/core/types.hpp"

#include <cstddef>
#include <expected>
#include <vector>

namespace avb::platform::windows {

  /// @brief Read the installed controller font into a bounded owned byte buffer.
  ///
  /// Only the system font file is read; no font is downloaded or redistributed.
  /// The returned vector owns its bytes independently of the closed input file.
  /// The caller must retain those bytes while the font atlas borrows them.
  /// Missing, empty, oversized, unreadable, or unallocatable data becomes a typed
  /// startup error. Successful reading does not itself validate glyph coverage.
  [[nodiscard]] std::expected<std::vector<std::byte>, core::AppError>
  load_controller_font();

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_CONTROLLER_FONT_HPP
