/// @file
/// @brief Portable controller snapshot and pure projection from Application-owned facts.
#ifndef AVB0795_APP_CONTROLLER_VIEW_STATE_HPP
#define AVB0795_APP_CONTROLLER_VIEW_STATE_HPP

#include "avb/core/types.hpp"
#include "avb/i18n/language.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace avb::core {
  class ApplicationState;
}

namespace avb::app {

  struct UserConfig;

  /// @brief Snapshot availability plus borrowed static text explaining disabled controls.
  /// This presentation hint is not authorization: commands still undergo admission
  /// when consumed, because the originating snapshot may have become stale.
  struct ControlState {
    bool enabled{};
    std::string_view explanation;
  };

  /// Owning source identity/label and precomputed row selection policy.
  struct ControllerSourceRow {
    core::SourceId id;
    std::string_view kind_glyph;
    std::string label;
    bool selected{};
    ControlState select;
    core::SourceKind kind{core::SourceKind::Display};
    std::string native_handle_text;
  };

  /// Last reported runtime bridge choice before user-facing text projection.
  struct BridgeFeedback {
    core::BridgeMode mode{core::BridgeMode::GpuInterop};
    core::BridgeReason reason{core::BridgeReason::None};
  };

  /// Feedback scoped to one published runtime, reset on publication and release.
  struct RuntimeFeedback {
    core::CaptureExclusionStatus overlay_exclusion{
      core::CaptureExclusionStatus::NotAttempted};
    std::optional<BridgeFeedback> bridge;
    core::RuntimeDrainStatus drain{core::RuntimeDrainStatus::Normal};
    /// Validated presented source content, cleared on lifecycle/source invalidation.
    std::optional<core::Extent> content_extent;
  };

  /// Portable bridge status rendered by UI without probing graphics capabilities.
  struct BridgeView {
    core::BridgeMode mode{core::BridgeMode::GpuInterop};
    core::BridgeReason reason{core::BridgeReason::None};
    std::string_view status_text;
  };

  /// Application command availability for the current snapshot.
  struct ControllerControls {
    ControlState refresh;
    ControlState auto_refresh;
    ControlState start;
    ControlState stop;
    ControlState editable;
    ControlState pass_through;
    ControlState fps;
    ControlState opacity;
    ControlState wheel_zoom;
    ControlState region;
    ControlState cancel_region_selection;
    ControlState bordered;
    ControlState borderless;
    ControlState replace_hotkey;
    ControlState browser_capture_keep_alive;
    ControlState language;
  };

  /// @brief Self-contained presentation snapshot with no runtime/service/native access.
  /// Dynamic source identities, labels and hotkey text are owned; string_views refer
  /// to static catalog text when produced by build_controller_view(). Copying that
  /// projection does not retain its source catalog or domain state.
  /// session_id is the sole generation source for session-specific widget commands;
  /// source-specific drafts additionally bind selected_source_id and content_extent.
  /// requested language and effective language are separate so System remains visible.
  struct ControllerViewState {
    core::SessionId session_id;
    i18n::LanguagePreference language_preference{
      i18n::LanguagePreference::System};
    i18n::Language effective_language{i18n::Language::English};
    std::vector<ControllerSourceRow> sources;
    std::string_view status_text;
    std::optional<std::string_view> session_error_text;
    core::ReplicaSettings settings;
    std::string hotkey_display_text;
    bool hotkey_binding_confirmed{};
    std::optional<BridgeView> bridge;
    std::optional<std::string_view> application_notice_text;
    core::RuntimeDrainStatus drain_status{core::RuntimeDrainStatus::Normal};
    std::optional<std::string_view> color_warning;
    std::optional<std::string_view> controller_exclusion_warning;
    std::optional<std::string_view> overlay_exclusion_warning;
    std::optional<std::string_view> drain_warning;
    std::string_view extensions_scope_text;
    ControllerControls controls;
    bool auto_refresh_enabled{};
    bool auto_refresh_paused{};
    /// Exact source identity and presented content bounds used to scope coordinate drafts.
    std::optional<core::SourceId> selected_source_id;
    std::optional<core::Extent> content_extent;
  };

  /// @brief Project one coherent set of supplied facts into owning rows and static text.
  /// runtime_present/stop_requested distinguish idle Error from retained Error drain;
  /// the latter disables replica mutation even though both share a lifecycle value.
  /// Language availability is supplied independently so it can remain usable during
  /// ordinary replica cleanup and close only with application shutdown.
  /// @pre The supplied generation and runtime/config facts describe the same session
  /// snapshot; inputs remain stable for this synchronous call. No locking or native
  /// probing is performed here.
  /// @return A projection independent of input span/state lifetime; building owned
  /// strings and rows may throw before the caller publishes the returned value.
  [[nodiscard]] ControllerViewState build_controller_view(
    const core::ApplicationState &state, core::SessionId current_session_id, std::span<const core::SourceDescriptor> sources, core::PlatformCapabilities capabilities, const UserConfig &config, bool hotkey_binding_confirmed, core::CaptureExclusionStatus controller_exclusion, const RuntimeFeedback &runtime_feedback, bool runtime_present, bool stop_requested, std::optional<core::AppError> application_notice = std::nullopt, i18n::Language language = i18n::Language::English, bool language_changes_allowed = true);

}    // namespace avb::app

#endif    // AVB0795_APP_CONTROLLER_VIEW_STATE_HPP
