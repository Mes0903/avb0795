/// @file
/// @brief Portable identities, settings and diagnostics shared across layer boundaries.
/// Native handles and resource lifetimes belong to platform/render owners.
#ifndef AVB0795_CORE_TYPES_HPP
#define AVB0795_CORE_TYPES_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace avb::core {

  /// @brief Generation token used to reject commands targeting a replaced session.
  /// Copy the token from the current view; its numeric value is not a runtime
  /// resource identifier and default construction does not name the initial session.
  struct SessionId {
    std::uint64_t value{};

    friend bool operator==(const SessionId &, const SessionId &) = default;
  };

  /// @brief Owned opaque catalog identity with no native ownership lease.
  /// String equality permits snapshot lookup, but retaining this value does not
  /// keep a source alive or make a stale observation safe to capture.
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

  /// @brief Source-image pixel rectangle with a top-left origin and half-open edges.
  /// Captures use this value to retain a viewing region independently of replica
  /// placement, scale and DPI. Positive width/height and containment are checked
  /// at command admission; aggregate construction also represents invalid input
  /// for validation. Arithmetic consumers use remaining extent to prevent overflow.
  struct PixelRegion {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    friend bool operator==(const PixelRegion &, const PixelRegion &) = default;
  };

  /// @brief Check a complete source-pixel rectangle against current content bounds.
  /// This pure check keeps invalid typed commands outside native settings apply;
  /// zero dimensions and rectangles crossing an edge fail as one value.
  [[nodiscard]] bool valid_region(PixelRegion region, Extent content) noexcept;

  /// @brief Clip a requested source region to current content after source resize.
  /// Empty intersections return nullopt, allowing the caller to restore full view.
  /// The requested origin remains fixed; subtraction bounds width/height safely
  /// even when a caller supplies values near the unsigned integer limit.
  [[nodiscard]] std::optional<PixelRegion>
  intersect_region(PixelRegion region, Extent content) noexcept;

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

  /// @brief Fixed-size failure value with optional numeric native detail, never dynamic text.
  /// Code drives portable policy and localization; detail preserves adapter-specific
  /// diagnostics without making user-facing wording part of the failure contract.
  struct AppError {
    AppErrorCode code{};
    std::optional<std::int64_t> native_detail{};

    /// Derive retryability from the canonical code, without storing duplicate state.
    [[nodiscard]] constexpr bool retryable() const noexcept
    {
      return app_error_retryable(code);
    }

    /// @brief Borrow a program-lifetime diagnostic label suitable for structured logging.
    /// Labels are untranslated and exclude native_detail; unknown codes use a
    /// stable fallback rather than allocating a message.
    [[nodiscard]] std::string_view diagnostic_label() const noexcept;

    friend bool operator==(const AppError &, const AppError &) = default;
  };

  /// @brief Owned catalog snapshot for selection/display; selecting it does not start capture.
  /// Extent and color are observations at enumeration time, not guarantees about
  /// the next frame. Strings remain valid independently of the enumerator's storage.
  struct SourceDescriptor {
    SourceId id;
    SourceKind kind{SourceKind::Display};
    std::string display_label;
    Extent extent;
    ColorSupport color_support{ColorSupport::UnknownAssumeSdr};
    /// @brief Platform-formatted diagnostic text, owned with the catalog snapshot.
    /// Window sources expose platform-formatted handle text here. Selection and
    /// runtime lookup continue through SourceId; this label conveys no native access.
    std::string native_handle_text;

    friend bool operator==(const SourceDescriptor &,
                           const SourceDescriptor &) = default;
  };

  /// Experimental portable preferences that remain disabled unless opted in.
  struct ExtensionSettings {
    bool browser_capture_keep_alive{};

    friend bool operator==(const ExtensionSettings &,
                           const ExtensionSettings &) = default;
  };

  /// Initial wheel zoom increment and local recovery value for malformed preferences.
  inline constexpr int default_wheel_zoom_percent = 5;

  /// Check the portable integer percentage before any runtime settings transaction.
  [[nodiscard]] constexpr bool valid_wheel_zoom_percent(int value) noexcept
  {
    return value >= 1 && value <= 25;
  }

  /// @brief Portable candidate or accepted settings, applied as a whole to a runtime.
  /// The value itself performs no validation or persistence. Application may persist
  /// accepted mode, border, cap, opacity, wheel zoom and extension preferences; crop and region
  /// selection remain session state, with temporary native overrides kept separate.
  struct ReplicaSettings {
    OverlayMode mode{OverlayMode::Editable};
    BorderPreference border{BorderPreference::Bordered};
    FpsCap fps_cap{FpsCap::Fps60};
    ExtensionSettings extensions{};
    int opacity_percent{100};
    /// Accepted percent increase per positive wheel notch; native geometry is session-only.
    int wheel_zoom_percent{default_wheel_zoom_percent};
    /// Session-only crop in source content pixels; nullopt presents the full frame.
    std::optional<PixelRegion> region{};
    /// Transient full-source selection request; persisted preferences remain unchanged.
    bool selecting_region{};

    friend bool operator==(const ReplicaSettings &,
                           const ReplicaSettings &) = default;
  };

  /// @brief Discovery policy; system selection starts only through explicit intent.
  enum class SourceSelectionMethod { Catalog, SystemPicker };

  /// @brief Pre-runtime behavior capabilities used to admit commands and explain controls.
  /// An available capability does not guarantee that a later native operation
  /// succeeds; bridge selection depends on actual constructed graphics resources.
  /// Composition supplies observed capabilities. Opacity defaults to the historical
  /// supported behavior; unsupported compositions retain saved preferences separately.
  struct PlatformCapabilities {
    bool display_capture{};
    bool window_capture{};
    bool pass_through{};
    bool borderless_resize{};
    bool global_hotkey{};
    bool capture_exclusion{};
    bool browser_capture_keep_alive{};
    SourceSelectionMethod source_selection_method{SourceSelectionMethod::Catalog};
    bool always_on_top{};
    bool window_opacity{true};

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
