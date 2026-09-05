/// @file
/// @brief Installed Windows font loading for the independent controller UI.
/// Native implementation details remain below the portable application ports.

#ifndef AIRELINNA_PLATFORM_WINDOWS_CONTROLLER_FONT_HPP
#define AIRELINNA_PLATFORM_WINDOWS_CONTROLLER_FONT_HPP

#include "airelinna/core/types.hpp"

#include <cstddef>
#include <expected>
#include <vector>

namespace airelinna::platform::windows {

  // Microsoft JhengHei is a Windows 11 core font. Only read the installed
  // system copy; no font is redistributed or downloaded by the application.
  /// Read a bounded owned byte buffer; the controller host retains it for the font atlas.
  /// Startup receives a typed error if the installed font cannot be loaded.
  [[nodiscard]] std::expected<std::vector<std::byte>, core::AppError>
  load_controller_font();

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_CONTROLLER_FONT_HPP
