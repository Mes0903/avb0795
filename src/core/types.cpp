/// @file
/// @brief Stable diagnostic labels for the portable error taxonomy.
#include "avb/core/types.hpp"

#include <algorithm>

namespace avb::core {

  bool valid_region(const PixelRegion region, const Extent content) noexcept
  {
    return region.width > 0 && region.height > 0 && region.x < content.width &&
           region.y < content.height && region.width <= content.width - region.x &&
           region.height <= content.height - region.y;
  }

  std::optional<PixelRegion> intersect_region(const PixelRegion region,
                                             const Extent content) noexcept
  {
    if (region.width == 0 || region.height == 0 || region.x >= content.width ||
        region.y >= content.height) {
      return std::nullopt;
    }
    return PixelRegion{region.x, region.y,
                       (std::min)(region.width, content.width - region.x),
                       (std::min)(region.height, content.height - region.y)};
  }

  std::string_view AppError::diagnostic_label() const noexcept
  {
    switch (code) {
    case AppErrorCode::InvalidCommand:
      return "invalid_command";
    case AppErrorCode::InvalidValue:
      return "invalid_value";
    case AppErrorCode::ConfigReadFailure:
      return "config_read_failure";
    case AppErrorCode::ConfigWriteFailure:
      return "config_write_failure";
    case AppErrorCode::SourceCatalogFailure:
      return "source_catalog_failure";
    case AppErrorCode::PermissionDenied:
      return "permission_denied";
    case AppErrorCode::SourceUnavailable:
      return "source_unavailable";
    case AppErrorCode::HotkeyConflict:
      return "hotkey_conflict";
    case AppErrorCode::HotkeyCleanupFailure:
      return "hotkey_cleanup_failure";
    case AppErrorCode::HotkeyRegistrationLost:
      return "hotkey_registration_lost";
    case AppErrorCode::UnsupportedWindowClass:
      return "unsupported_window_class";
    case AppErrorCode::UnsupportedGraphics:
      return "unsupported_graphics";
    case AppErrorCode::RuntimeConstructionFailure:
      return "runtime_construction_failure";
    case AppErrorCode::PlatformOperationFailure:
      return "platform_operation_failure";
    case AppErrorCode::RuntimeStateLost:
      return "runtime_state_lost";
    }

    return "unknown_application_error";
  }

}    // namespace avb::core
