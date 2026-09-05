/// @file Pure controller projection: all runtime facts arrive as portable values.
#include "airelinna/app/controller_view_state.hpp"

#include "airelinna/app/config_store.hpp"
#include "airelinna/core/application_state.hpp"
#include "airelinna/core/hotkey.hpp"

#include <optional>
#include <string_view>

namespace airelinna::app {
  namespace {

    constexpr std::string_view stopping_explanation{
      "The current replica is stopping."};
    constexpr std::string_view lifecycle_explanation{
      "This action is unavailable in the current lifecycle."};
    constexpr std::string_view capability_explanation{
      "This capability is unavailable."};
    constexpr std::string_view missing_source_explanation{
      "Select a source first."};

    /// Shared disable precedence for every projected control in this snapshot.
    struct ControlContext {
      bool stop_or_drain{};
    };

    [[nodiscard]] constexpr ControlState
    control(const ControlContext context, bool lifecycle_allowed, bool capability_available = true, bool source_missing = false) noexcept
    {
      // Show the first actionable reason in order: drain, lifecycle, capability,
      // selection. Native cleanup always disables mutation regardless of mode.
      if (context.stop_or_drain) {
        return {.enabled = false, .explanation = stopping_explanation};
      }
      if (!lifecycle_allowed) {
        return {.enabled = false, .explanation = lifecycle_explanation};
      }
      if (!capability_available) {
        return {.enabled = false, .explanation = capability_explanation};
      }
      if (source_missing) {
        return {.enabled = false, .explanation = missing_source_explanation};
      }
      return {.enabled = true};
    }

    [[nodiscard]] constexpr bool
    supports_source(core::SourceKind kind,
                    core::PlatformCapabilities capabilities) noexcept
    {
      return kind == core::SourceKind::Display ? capabilities.display_capture
                                               : capabilities.window_capture;
    }

    [[nodiscard]] constexpr std::string_view
    source_glyph(core::SourceKind kind) noexcept
    {
      return kind == core::SourceKind::Display ? "display" : "window";
    }

    [[nodiscard]] constexpr std::string_view
    lifecycle_text(core::SessionLifecycle lifecycle) noexcept
    {
      switch (lifecycle) {
      case core::SessionLifecycle::Idle:
        return "Idle";
      case core::SessionLifecycle::Starting:
        return "Starting";
      case core::SessionLifecycle::Capturing:
        return "Capturing";
      case core::SessionLifecycle::Stopping:
        return "Stopping";
      case core::SessionLifecycle::Interrupted:
        return "Capture interrupted";
      case core::SessionLifecycle::RecoveringGraphics:
        return "Recovering graphics";
      case core::SessionLifecycle::Error:
        return "Error";
      }
      return "Error";
    }

    [[nodiscard]] constexpr std::string_view
    bridge_text(const BridgeFeedback feedback) noexcept
    {
      switch (feedback.reason) {
      case core::BridgeReason::None:
        return feedback.mode == core::BridgeMode::GpuInterop
                 ? "GPU interop active."
                 : "CPU copy bridge active.";
      case core::BridgeReason::AdapterLuidUnavailableOrMismatch:
        return "CPU copy: graphics adapters could not be matched.";
      case core::BridgeReason::ExternalImageUnsupported:
        return "CPU copy: external images are unsupported.";
      case core::BridgeReason::RequiredD3d11InterfacesUnavailable:
        return "CPU copy: required D3D11 interfaces are unavailable.";
      case core::BridgeReason::SharedFenceOrTimelineSemaphoreUnavailable:
        return "CPU copy: shared synchronization is unavailable.";
      case core::BridgeReason::TimelineFenceImportUnavailable:
        return "CPU copy: timeline fence import is unavailable.";
      case core::BridgeReason::Win32ImportProcedureUnavailable:
        return "CPU copy: Win32 import procedures are unavailable.";
      case core::BridgeReason::CompatibleMemoryTypeUnavailable:
        return "CPU copy: no compatible memory type is available.";
      case core::BridgeReason::InteropOperationFailure:
        return "CPU copy: a GPU interop operation failed.";
      }
      return "CPU copy bridge active.";
    }

    [[nodiscard]] constexpr std::optional<std::string_view>
    color_warning(core::ColorSupport support) noexcept
    {
      switch (support) {
      case core::ColorSupport::Sdr:
        return std::nullopt;
      case core::ColorSupport::HdrDetectedUnsupported:
        return "HDR was detected but is unsupported.";
      case core::ColorSupport::UnknownAssumeSdr:
        return "Color detection is unavailable; SDR is assumed.";
      }
      return std::nullopt;
    }

    [[nodiscard]] constexpr std::optional<std::string_view>
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

  }    // namespace

  ControllerViewState build_controller_view(
    const core::ApplicationState &state, core::SessionId current_session_id, std::span<const core::SourceDescriptor> sources, core::PlatformCapabilities capabilities, const UserConfig &config, bool hotkey_binding_confirmed, core::CaptureExclusionStatus controller_exclusion, const RuntimeFeedback &runtime_feedback, bool runtime_present, bool stop_requested, std::optional<core::AppError> application_notice)
  {
    const auto &session = state.current_session();
    const auto lifecycle = session.lifecycle();
    // Error alone is insufficient: failed construction has no runtime and can
    // restart, while terminal Error with a retained runtime must finish drain.
    const bool stop_or_drain =
      stop_requested || lifecycle == core::SessionLifecycle::Stopping ||
      (runtime_present && lifecycle == core::SessionLifecycle::Error);
    const ControlContext context{.stop_or_drain = stop_or_drain};
    const bool inactive = !runtime_present &&
                          (lifecycle == core::SessionLifecycle::Idle ||
                           lifecycle == core::SessionLifecycle::Error);
    const bool interrupted = runtime_present &&
                             lifecycle == core::SessionLifecycle::Interrupted;
    const bool active =
      runtime_present &&
      (lifecycle == core::SessionLifecycle::Capturing || interrupted ||
       lifecycle == core::SessionLifecycle::RecoveringGraphics);
    const bool select_or_start_allowed = inactive || interrupted;
    const bool settings_allowed = inactive || active;

    ControllerViewState view{
      .session_id = current_session_id,
      .status_text = lifecycle_text(lifecycle),
      .settings = session.settings(),
      .hotkey_display_text = core::hotkey_display_text(config.hotkey),
      .hotkey_binding_confirmed = hotkey_binding_confirmed,
      .application_notice_text =
        application_notice.transform(
          [](const core::AppError error) { return error.user_message(); }),
      .drain_status = runtime_present ? runtime_feedback.drain
                                      : core::RuntimeDrainStatus::Normal,
    };

    if (lifecycle == core::SessionLifecycle::Error) {
      view.session_error_text = "Replica session failed.";
    }

    // Copy dynamic labels/identities so this snapshot never borrows catalog storage.
    view.sources.reserve(sources.size());
    for (const auto &source : sources) {
      const bool selected = session.selected_source() &&
                            session.selected_source()->id == source.id;
      view.sources.push_back(ControllerSourceRow{
        .id = source.id,
        .kind_glyph = source_glyph(source.kind),
        .label = source.display_label,
        .selected = selected,
        .select = control(context, select_or_start_allowed,
                          supports_source(source.kind, capabilities)),
      });
    }

    // Static capabilities cannot predict the path chosen by actual native devices.
    // Suppress runtime-scoped feedback once that owner has been released.
    if (runtime_present && runtime_feedback.bridge) {
      view.bridge = BridgeView{
        .mode = runtime_feedback.bridge->mode,
        .reason = runtime_feedback.bridge->reason,
        .status_text = bridge_text(*runtime_feedback.bridge),
      };
    }

    if (session.selected_source()) {
      view.color_warning =
        color_warning(session.selected_source()->color_support);
    }
    // Controller exclusion survives replica replacement; overlay exclusion and
    // stalled-drain warnings belong only to the currently retained runtime.
    view.controller_exclusion_warning = exclusion_warning(
      controller_exclusion,
      "Controller capture exclusion is unsupported.",
      "Controller capture exclusion failed.");
    if (runtime_present) {
      view.overlay_exclusion_warning = exclusion_warning(
        runtime_feedback.overlay_exclusion,
        "Overlay capture exclusion is unsupported.",
        "Overlay capture exclusion failed.");
      if (runtime_feedback.drain == core::RuntimeDrainStatus::Stalled) {
        view.drain_warning = "Native resource cleanup has stalled.";
      }
    }

    const bool has_source = session.selected_source().has_value();
    const bool source_capability =
      !has_source || supports_source(session.selected_source()->kind,
                                     capabilities);
    view.controls = ControllerControls{
      .refresh = control(context, inactive),
      .start = control(context, select_or_start_allowed, source_capability,
                       !has_source),
      .stop = control(context, active),
      .editable = control(context, settings_allowed),
      .pass_through =
        control(context, settings_allowed, capabilities.pass_through),
      .fps = control(context, settings_allowed),
      .bordered = control(context, settings_allowed),
      .borderless =
        control(context, settings_allowed, capabilities.borderless_resize),
      .replace_hotkey =
        control(context, settings_allowed, capabilities.global_hotkey),
    };
    return view;
  }

}    // namespace airelinna::app
