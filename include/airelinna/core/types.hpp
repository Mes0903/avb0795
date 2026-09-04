#ifndef AIRELINNA_CORE_TYPES_HPP
#define AIRELINNA_CORE_TYPES_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace airelinna::core {

struct SessionId {
  std::uint64_t value{};

  friend bool operator==(const SessionId &, const SessionId &) = default;
};

struct SourceId {
  std::string value;

  friend bool operator==(const SourceId &, const SourceId &) = default;
};

struct Extent {
  std::uint32_t width{};
  std::uint32_t height{};

  friend bool operator==(const Extent &, const Extent &) = default;
};

enum class SourceKind { Display, Window };

enum class SessionLifecycle {
  Idle,
  Starting,
  Capturing,
  Stopping,
  Interrupted,
  RecoveringGraphics,
  Error,
};

enum class OverlayMode { Editable, PassThrough };
enum class BorderPreference { Bordered, Borderless };
enum class FpsCap { Fps30, Fps60, Fps144, Unlimited };
enum class ColorSupport { Sdr, HdrDetectedUnsupported, UnknownAssumeSdr };
enum class ExitDisposition { Continue, Clean, AbandonProcess };

enum class CaptureExclusionStatus {
  NotAttempted,
  Excluded,
  Unsupported,
  Failed,
};

enum class RuntimeDrainStatus { Normal, Stalled };
enum class BridgeMode { CpuCopy, GpuInterop };

enum class BridgeReason {
  None,
  AdapterLuidUnavailableOrMismatch,
  ExternalImageUnsupported,
  RequiredD3d11InterfacesUnavailable,
  SharedFenceOrTimelineSemaphoreUnavailable,
  TimelineFenceImportUnavailable,
  Win32ImportProcedureUnavailable,
  CompatibleMemoryTypeUnavailable,
  InteropOperationFailure,
};

enum class AppErrorCode {
  InvalidCommand,
  InvalidValue,
  ConfigReadFailure,
  ConfigWriteFailure,
  SourceCatalogFailure,
  PermissionDenied,
  SourceUnavailable,
  HotkeyConflict,
  HotkeyCleanupFailure,
  HotkeyRegistrationLost,
  UnsupportedWindowClass,
  UnsupportedGraphics,
  RuntimeConstructionFailure,
  PlatformOperationFailure,
  RuntimeStateLost,
};

[[nodiscard]] constexpr bool app_error_retryable(AppErrorCode code) noexcept {
  switch (code) {
  case AppErrorCode::ConfigReadFailure:
  case AppErrorCode::ConfigWriteFailure:
  case AppErrorCode::SourceCatalogFailure:
  case AppErrorCode::PermissionDenied:
  case AppErrorCode::SourceUnavailable:
  case AppErrorCode::HotkeyConflict:
  case AppErrorCode::HotkeyCleanupFailure:
  case AppErrorCode::HotkeyRegistrationLost:
  case AppErrorCode::RuntimeConstructionFailure:
  case AppErrorCode::PlatformOperationFailure:
    return true;

  case AppErrorCode::InvalidCommand:
  case AppErrorCode::InvalidValue:
  case AppErrorCode::UnsupportedWindowClass:
  case AppErrorCode::UnsupportedGraphics:
  case AppErrorCode::RuntimeStateLost:
    return false;
  }

  return false;
}

struct AppError {
  AppErrorCode code{};
  std::optional<std::int64_t> native_detail{};

  [[nodiscard]] constexpr bool retryable() const noexcept {
    return app_error_retryable(code);
  }

  [[nodiscard]] std::string_view user_message() const noexcept;
  [[nodiscard]] std::string_view diagnostic_label() const noexcept;

  friend bool operator==(const AppError &, const AppError &) = default;
};

struct SourceDescriptor {
  SourceId id;
  SourceKind kind{SourceKind::Display};
  std::string display_label;
  Extent extent;
  ColorSupport color_support{ColorSupport::UnknownAssumeSdr};

  friend bool operator==(const SourceDescriptor &,
                         const SourceDescriptor &) = default;
};

struct ReplicaSettings {
  OverlayMode mode{OverlayMode::Editable};
  BorderPreference border{BorderPreference::Bordered};
  FpsCap fps_cap{FpsCap::Fps60};

  friend bool operator==(const ReplicaSettings &,
                         const ReplicaSettings &) = default;
};

struct PlatformCapabilities {
  bool display_capture{};
  bool window_capture{};
  bool pass_through{};
  bool borderless_resize{};
  bool global_hotkey{};
  bool capture_exclusion{};

  friend bool operator==(const PlatformCapabilities &,
                         const PlatformCapabilities &) = default;
};

struct HotkeyModifiers {
  bool control{};
  bool alt{};
  bool shift{};
  bool meta{};

  friend bool operator==(const HotkeyModifiers &,
                         const HotkeyModifiers &) = default;
};

enum class HotkeyKey {
  A,
  B,
  C,
  D,
  E,
  F,
  G,
  H,
  I,
  J,
  K,
  L,
  M,
  N,
  O,
  P,
  Q,
  R,
  S,
  T,
  U,
  V,
  W,
  X,
  Y,
  Z,
  Digit0,
  Digit1,
  Digit2,
  Digit3,
  Digit4,
  Digit5,
  Digit6,
  Digit7,
  Digit8,
  Digit9,
  F1,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,
};

struct HotkeyChord {
  HotkeyModifiers modifiers;
  HotkeyKey key{HotkeyKey::A};

  friend bool operator==(const HotkeyChord &, const HotkeyChord &) = default;
};

} // namespace airelinna::core

#endif // AIRELINNA_CORE_TYPES_HPP
