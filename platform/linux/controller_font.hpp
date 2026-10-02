/// @file
/// @brief Installed Linux controller font discovery with complete catalog coverage.
#ifndef AVB0795_LINUX_CONTROLLER_FONT_HPP
#define AVB0795_LINUX_CONTROLLER_FONT_HPP

#include "avb/core/types.hpp"
#include "avb/desktop/controller_font_data.hpp"
#include <expected>

namespace avb::linux_platform {
  /// @brief Load an installed Noto CJK match covering actual three-language UI text.
  /// Own the full bounded file bytes and preserve the matched collection index.
  /// Matching alone is insufficient: required printable catalog glyphs are checked
  /// against the actual selected face. Missing coverage, unreadable/oversized files
  /// or discovery failures return RuntimeConstructionFailure for startup diagnosis.
  /// No native discovery objects survive the synchronous call. Caller keeps the
  /// returned bytes unchanged through atlas teardown, including deferred baking.
  [[nodiscard]] std::expected<desktop::ControllerFontData, core::AppError> load_controller_font();
}    // namespace avb::linux_platform

#endif
