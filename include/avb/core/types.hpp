/// @file Portable identities, settings and diagnostics shared across layer boundaries.
/// Native handles and resource lifetimes belong to platform/render owners.
#ifndef AVB0795_CORE_TYPES_HPP
#define AVB0795_CORE_TYPES_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace avb::core {

  /// Generation token used to reject commands targeting a replaced session.
  struct SessionId {
    std::uint64_t value{};

    friend bool operator==(const SessionId &, const SessionId &) = default;
  };

  /// Opaque catalog identity; it carries no native handle or ownership lease.
  struct SourceId {
    std::string value;

    friend bool operator==(const SourceId &, const SourceId &) = default;
  };

  /// Pixel dimensions; zero is representable and must be interpreted by consumers.
  struct Extent {
    std::uint32_t width{};
    std::uint32_t height{};

    friend bool operator==(const Extent &, const Extent &) = default;
  };

  /// Source category used by catalog filtering and capability-driven controls.
  enum class SourceKind { Display,
                          Window };

  /// Visible domain lifecycle, distinct from the native runtime's drain phase.
  enum class SessionLifecycle {
    Idle,
    Starting,
    Capturing,
    Stopping,
    Interrupted,
    RecoveringGraphics,
    Error,
  };

  /// Session-only input policy; PassThrough does not synthesize or forward clicks.
  enum class OverlayMode { Editable,
                           PassThrough };
  /// Persisted preference for native chrome around the replica content.
  enum class BorderPreference { Bordered,
                                Borderless };
  /// Maximum work-admission rate; Unlimited still requires pending frame/redraw work.
  enum class FpsCap { Fps30,
                      Fps60,
                      Fps144,
                      Unlimited };
  /// Catalog-time color observation; unsupported/unknown values drive UI warnings.
  enum class ColorSupport { Sdr,
                            HdrDetectedUnsupported,
                            UnknownAssumeSdr };
  /// Application's host instruction; AbandonProcess requires a non-unwinding exit.
  enum class ExitDisposition { Continue,
                               Clean,
                               AbandonProcess };

  /// Best-effort result of excluding one product window from capture.
  enum class CaptureExclusionStatus {
    NotAttempted,
    Excluded,
    Unsupported,
    Failed,
  };

  /// Drain progress feedback; Stalled never grants permission to destroy resources.
  enum class RuntimeDrainStatus { Normal,
                                  Stalled };
  /// Actual frame-transfer path reported by a constructed runtime.
  enum class BridgeMode { CpuCopy,
                          GpuInterop };

  /// First unsupported interop fact, or a later interop operation failure.
  /// None accompanies the GPU path; this is runtime feedback, not a UI capability.
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

  /// Canonical portable error taxonomy used by all application ports.
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

  /// Classify whether retry may help; this neither schedules retries nor grants
  /// permission to retry in the current lifecycle or native resource phase.
  [[nodiscard]] constexpr bool app_error_retryable(AppErrorCode code) noexcept
  {
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

  /// Fixed-size failure value with optional numeric native detail, never dynamic text.
  struct AppError {
    AppErrorCode code{};
    std::optional<std::int64_t> native_detail{};

    /// Derive retryability from the canonical code, without storing duplicate state.
    [[nodiscard]] constexpr bool retryable() const noexcept
    {
      return app_error_retryable(code);
    }

    /// Borrow a static user-facing explanation; no native/source text is included.
    [[nodiscard]] std::string_view user_message() const noexcept;
    /// Borrow a stable diagnostic label suitable for structured logging.
    [[nodiscard]] std::string_view diagnostic_label() const noexcept;

    friend bool operator==(const AppError &, const AppError &) = default;
  };

  /// Owned catalog snapshot for selection/display; selecting it does not start capture.
  struct SourceDescriptor {
    SourceId id;
    SourceKind kind{SourceKind::Display};
    std::string display_label;
    Extent extent;
    ColorSupport color_support{ColorSupport::UnknownAssumeSdr};

    friend bool operator==(const SourceDescriptor &,
                           const SourceDescriptor &) = default;
  };

  /// Experimental portable preferences that remain disabled unless opted in.
  struct ExtensionSettings {
    bool browser_capture_keep_alive{};

    friend bool operator==(const ExtensionSettings &,
                           const ExtensionSettings &) = default;
  };

  /// Portable settings transaction applied as a whole to the current runtime.
  struct ReplicaSettings {
    OverlayMode mode{OverlayMode::Editable};
    BorderPreference border{BorderPreference::Bordered};
    FpsCap fps_cap{FpsCap::Fps60};
    ExtensionSettings extensions{};

    friend bool operator==(const ReplicaSettings &,
                           const ReplicaSettings &) = default;
  };

  /// Pre-runtime behavior capabilities; excludes dynamic graphics bridge selection.
  struct PlatformCapabilities {
    bool display_capture{};
    bool window_capture{};
    bool pass_through{};
    bool borderless_resize{};
    bool global_hotkey{};
    bool capture_exclusion{};
    bool browser_capture_keep_alive{};

    friend bool operator==(const PlatformCapabilities &,
                           const PlatformCapabilities &) = default;
  };

  /// Independent modifier flags shared by parser, persistence and OS registration.
  struct HotkeyModifiers {
    bool control{};
    bool alt{};
    bool shift{};
    bool meta{};

    friend bool operator==(const HotkeyModifiers &,
                           const HotkeyModifiers &) = default;
  };

  /// Supported portable key inventory; adapters translate these values to native keys.
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

  /// Typed desired binding; OS registration success is separate Application state.
  struct HotkeyChord {
    HotkeyModifiers modifiers;
    HotkeyKey key{HotkeyKey::A};

    friend bool operator==(const HotkeyChord &, const HotkeyChord &) = default;
  };

}    // namespace avb::core

#endif    // AVB0795_CORE_TYPES_HPP
