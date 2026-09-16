/// @file Static user and diagnostic labels for the portable error taxonomy.
/// Keeping text here lets no-throw runtime events carry only fixed-size AppError values.
#include "avb/core/types.hpp"

namespace avb::core {

  std::string_view AppError::user_message() const noexcept
  {
    switch (code) {
    case AppErrorCode::InvalidCommand:
      return "That action is not available in the current state.";
    case AppErrorCode::InvalidValue:
      return "A provided value is invalid.";
    case AppErrorCode::ConfigReadFailure:
      return "The configuration could not be read.";
    case AppErrorCode::ConfigWriteFailure:
      return "The configuration could not be saved.";
    case AppErrorCode::SourceCatalogFailure:
      return "Capture sources could not be listed.";
    case AppErrorCode::PermissionDenied:
      return "Permission to capture the source was denied.";
    case AppErrorCode::SourceUnavailable:
      return "The selected capture source is unavailable.";
    case AppErrorCode::HotkeyConflict:
      return "The requested global hotkey is already in use.";
    case AppErrorCode::HotkeyCleanupFailure:
      return "The previous global hotkey could not be released.";
    case AppErrorCode::HotkeyRegistrationLost:
      return "The global hotkey registration was lost.";
    case AppErrorCode::UnsupportedWindowClass:
      return "The window configuration is unsupported.";
    case AppErrorCode::UnsupportedGraphics:
      return "The graphics configuration is unsupported.";
    case AppErrorCode::RuntimeConstructionFailure:
      return "The replica could not be started.";
    case AppErrorCode::PlatformOperationFailure:
      return "A platform operation failed.";
    case AppErrorCode::RuntimeStateLost:
      return "The active replica can no longer continue safely.";
    }

    return "An unknown application error occurred.";
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
