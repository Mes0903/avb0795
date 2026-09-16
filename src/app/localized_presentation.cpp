/// @file
/// @brief Translate portable status taxonomies into catalog keys and optional warnings.
#include "avb/app/localized_presentation.hpp"

#include "avb/app/controller_view_state.hpp"
#include "avb/i18n/catalog.hpp"

namespace avb::app {

  std::string_view
  error_text(core::AppErrorCode code, i18n::Language language) noexcept
  {
    using enum core::AppErrorCode;
    using enum i18n::TextId;
    switch (code) {
    case InvalidCommand:
      return i18n::text(ErrorInvalidCommand, language);
    case InvalidValue:
      return i18n::text(ErrorInvalidValue, language);
    case ConfigReadFailure:
      return i18n::text(ErrorConfigReadFailure, language);
    case ConfigWriteFailure:
      return i18n::text(ErrorConfigWriteFailure, language);
    case SourceCatalogFailure:
      return i18n::text(ErrorSourceCatalogFailure, language);
    case PermissionDenied:
      return i18n::text(ErrorPermissionDenied, language);
    case SourceUnavailable:
      return i18n::text(ErrorSourceUnavailable, language);
    case HotkeyConflict:
      return i18n::text(ErrorHotkeyConflict, language);
    case HotkeyCleanupFailure:
      return i18n::text(ErrorHotkeyCleanupFailure, language);
    case HotkeyRegistrationLost:
      return i18n::text(ErrorHotkeyRegistrationLost, language);
    case UnsupportedWindowClass:
      return i18n::text(ErrorUnsupportedWindowClass, language);
    case UnsupportedGraphics:
      return i18n::text(ErrorUnsupportedGraphics, language);
    case RuntimeConstructionFailure:
      return i18n::text(ErrorRuntimeConstructionFailure, language);
    case PlatformOperationFailure:
      return i18n::text(ErrorPlatformOperationFailure, language);
    case RuntimeStateLost:
      return i18n::text(ErrorRuntimeStateLost, language);
    }
    return i18n::text(ErrorUnknown, language);
  }

  std::string_view
  lifecycle_text(core::SessionLifecycle lifecycle,
                 i18n::Language language) noexcept
  {
    using enum core::SessionLifecycle;
    switch (lifecycle) {
    case Idle:
      return i18n::text(i18n::TextId::Idle, language);
    case Starting:
      return i18n::text(i18n::TextId::Starting, language);
    case Capturing:
      return i18n::text(i18n::TextId::Capturing, language);
    case Stopping:
      return i18n::text(i18n::TextId::Stopping, language);
    case Interrupted:
      return i18n::text(i18n::TextId::Interrupted, language);
    case RecoveringGraphics:
      return i18n::text(i18n::TextId::RecoveringGraphics, language);
    case Error:
      return i18n::text(i18n::TextId::Error, language);
    }
    return i18n::text(i18n::TextId::Error, language);
  }

  std::string_view
  source_kind_text(core::SourceKind kind, i18n::Language language) noexcept
  {
    return i18n::text(kind == core::SourceKind::Display
                        ? i18n::TextId::SourceDisplay
                        : i18n::TextId::SourceWindow,
                      language);
  }

  std::string_view
  bridge_text(BridgeFeedback feedback, i18n::Language language) noexcept
  {
    using enum core::BridgeReason;
    using enum i18n::TextId;
    // Preserve why fallback occurred even when the transfer mode alone would
    // produce a less actionable generic CPU-copy message.
    switch (feedback.reason) {
    case None:
      return i18n::text(feedback.mode == core::BridgeMode::GpuInterop
                          ? BridgeGpuInterop
                          : BridgeCpuCopy,
                        language);
    case AdapterLuidUnavailableOrMismatch:
      return i18n::text(BridgeAdapterMismatch, language);
    case ExternalImageUnsupported:
      return i18n::text(BridgeExternalImageUnsupported, language);
    case RequiredD3d11InterfacesUnavailable:
      return i18n::text(BridgeD3d11InterfacesUnavailable, language);
    case SharedFenceOrTimelineSemaphoreUnavailable:
      return i18n::text(BridgeSharedSyncUnavailable, language);
    case TimelineFenceImportUnavailable:
      return i18n::text(BridgeTimelineImportUnavailable, language);
    case Win32ImportProcedureUnavailable:
      return i18n::text(BridgeWin32ImportUnavailable, language);
    case CompatibleMemoryTypeUnavailable:
      return i18n::text(BridgeMemoryTypeUnavailable, language);
    case InteropOperationFailure:
      return i18n::text(BridgeInteropFailure, language);
    }
    return i18n::text(BridgeCpuCopy, language);
  }

  std::optional<std::string_view>
  color_warning(core::ColorSupport support, i18n::Language language) noexcept
  {
    switch (support) {
    case core::ColorSupport::Sdr:
      return std::nullopt;
    case core::ColorSupport::HdrDetectedUnsupported:
      return i18n::text(i18n::TextId::ColorHdrUnsupported, language);
    case core::ColorSupport::UnknownAssumeSdr:
      return i18n::text(i18n::TextId::ColorUnknownSdr, language);
    }
    return std::nullopt;
  }

  std::optional<std::string_view>
  exclusion_warning(core::CaptureExclusionStatus status,
                    std::string_view unsupported,
                    std::string_view failed) noexcept
  {
    switch (status) {
    case core::CaptureExclusionStatus::Unsupported:
      return unsupported;
    case core::CaptureExclusionStatus::Failed:
      return failed;
    case core::CaptureExclusionStatus::NotAttempted:
    case core::CaptureExclusionStatus::Excluded:
      return std::nullopt;
    }
    return std::nullopt;
  }

}    // namespace avb::app
