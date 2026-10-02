/// @file
/// @brief Immutable UTF-8 text borrowed by application projection and frontend widgets.
#ifndef AVB0795_I18N_CATALOG_HPP
#define AVB0795_I18N_CATALOG_HPP

#include "avb/i18n/language.hpp"

#include <cstddef>
#include <string_view>

namespace avb::i18n {

  /// @brief Semantic catalog keys whose ordinal order indexes the compiled table.
  /// Keep table rows aligned with this inventory; Count is a size sentinel rather
  /// than a text entry. Keys identify product text, not mutable translated strings.
  enum class TextId {
    UnknownText,
    ControllerTitle,
    GeneralSettings,
    LanguageLabel,
    FollowSystem,
    EffectiveLanguage,
    LanguageEnglish,
    LanguageTraditionalChinese,
    LanguageJapanese,
    LanguageChangesClosed,
    StatusLabel,
    SessionErrorLabel,
    NoticeLabel,
    ColorLabel,
    ControllerExclusionLabel,
    OverlayExclusionLabel,
    CleanupLabel,
    BridgeLabel,
    Sources,
    Refresh,
    SourceType,
    SourceName,
    AutoRefresh,
    AutoRefreshPaused,
    ChooseDisplay,
    ChooseWindow,
    FreshSourcePicker,
    AcquisitionPermission,
    AcquisitionPreparing,
    AcquisitionCancelling,
    CancelAcquisition,
    Start,
    Stop,
    Mode,
    Editable,
    PassThrough,
    Opacity,
    OpacityHelp,
    WheelZoom,
    WheelZoomHelp,
    SourceRegion,
    SourceContentSize,
    SelectRegion,
    CancelRegion,
    ResetRegion,
    RegionHelp,
    RegionAwaitingContent,
    FrameRate,
    Fps30,
    Fps60,
    Fps144,
    Unlimited,
    Border,
    Bordered,
    Borderless,
    Extensions,
    KeepAlive,
    KeepAliveHelp,
    ExtensionsScope,
    Hotkey,
    CurrentHotkey,
    RecordHotkey,
    RecordHotkeyPrompt,
    ReleaseRecordingKeys,
    ConfirmHotkey,
    CancelHotkey,
    BindingLabel,
    Confirmed,
    Unconfirmed,
    Quit,
    Minimize,
    ControlStopping,
    ControlLifecycle,
    ControlCapability,
    ControlMissingSource,
    SourceDisplay,
    SourceWindow,
    Idle,
    Starting,
    Capturing,
    Stopping,
    Interrupted,
    RecoveringGraphics,
    Error,
    SessionFailed,
    BridgeGpuInterop,
    BridgeCpuCopy,
    BridgeAdapterMismatch,
    BridgeExternalImageUnsupported,
    BridgeD3d11InterfacesUnavailable,
    BridgeSharedSyncUnavailable,
    BridgeTimelineImportUnavailable,
    BridgeWin32ImportUnavailable,
    BridgeMemoryTypeUnavailable,
    BridgeInteropFailure,
    ColorHdrUnsupported,
    ColorUnknownSdr,
    ControllerExclusionUnsupported,
    ControllerExclusionFailed,
    OverlayExclusionUnsupported,
    OverlayExclusionFailed,
    DrainStalled,
    ErrorInvalidCommand,
    ErrorInvalidValue,
    ErrorConfigReadFailure,
    ErrorConfigWriteFailure,
    ErrorSourceCatalogFailure,
    ErrorPermissionDenied,
    ErrorSourceUnavailable,
    ErrorHotkeyConflict,
    ErrorHotkeyCleanupFailure,
    ErrorHotkeyRegistrationLost,
    ErrorUnsupportedWindowClass,
    ErrorUnsupportedGraphics,
    ErrorRuntimeConstructionFailure,
    ErrorPlatformOperationFailure,
    ErrorRuntimeStateLost,
    ErrorUnknown,
    Count,
  };

  inline constexpr std::size_t text_count =
    static_cast<std::size_t>(TextId::Count);

  /// @brief Borrow a complete UTF-8 catalog string with program lifetime.
  /// Views refer to null-terminated literals, allowing widgets to use data() while
  /// length-aware consumers use the view directly. No allocation, runtime loading
  /// or mutable locale is involved; independent lookups require no synchronization.
  /// @return The requested translation, English for an invalid language, or the
  /// English unknown-text fallback for any invalid key (including Count).
  [[nodiscard]] std::string_view text(TextId id, Language language) noexcept;

}    // namespace avb::i18n

#endif    // AVB0795_I18N_CATALOG_HPP
