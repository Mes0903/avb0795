/// @file Portable controller snapshot and pure projection from Application-owned facts.
#ifndef AVB0795_APP_CONTROLLER_VIEW_STATE_HPP
#define AVB0795_APP_CONTROLLER_VIEW_STATE_HPP

#include "avb/core/types.hpp"

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

  /// Availability plus a static explanation; widgets display policy instead of recomputing it.
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
    ControlState start;
    ControlState stop;
    ControlState editable;
    ControlState pass_through;
    ControlState fps;
    ControlState bordered;
    ControlState borderless;
    ControlState replace_hotkey;
    ControlState browser_capture_keep_alive;
  };

  /// Value snapshot borrowed as const by UI; it contains no runtime/service/native access.
  /// Dynamic labels and hotkey text are owned; string_views refer to static text.
  /// session_id is the sole generation source for session-specific widget commands.
  struct ControllerViewState {
    core::SessionId session_id;
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
    std::string_view extensions_scope_text{"Window captures only"};
    ControllerControls controls;
  };

  /// Purely project explicit facts into labels, warnings and command availability.
  /// runtime_present/stop_requested distinguish idle Error from retained Error drain.
  /// Returned data has no dependency on the lifetime of the input source span.
  [[nodiscard]] ControllerViewState build_controller_view(
    const core::ApplicationState &state, core::SessionId current_session_id, std::span<const core::SourceDescriptor> sources, core::PlatformCapabilities capabilities, const UserConfig &config, bool hotkey_binding_confirmed, core::CaptureExclusionStatus controller_exclusion, const RuntimeFeedback &runtime_feedback, bool runtime_present, bool stop_requested, std::optional<core::AppError> application_notice = std::nullopt);

}    // namespace avb::app

#endif    // AVB0795_APP_CONTROLLER_VIEW_STATE_HPP
