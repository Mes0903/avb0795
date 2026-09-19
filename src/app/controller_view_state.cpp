/// @file
/// @brief Pure controller projection: all runtime facts arrive as portable values.
#include "avb/app/controller_view_state.hpp"

#include "avb/app/config_store.hpp"
#include "avb/app/localized_presentation.hpp"
#include "avb/core/application_state.hpp"
#include "avb/core/hotkey.hpp"
#include "avb/i18n/catalog.hpp"

#include <optional>
#include <string_view>

namespace avb::app {
  namespace {

    /// @brief Shared disable precedence for replica controls in this snapshot.
    struct ControlContext {
      bool stop_or_drain{};
      i18n::Language language{i18n::Language::English};
    };

    [[nodiscard]] ControlState
    control(const ControlContext context, bool lifecycle_allowed, bool capability_available = true, bool source_missing = false) noexcept
    {
      // Show the first actionable reason in order: drain, lifecycle, capability,
      // selection. Native cleanup always disables replica mutation regardless of mode.
      if (context.stop_or_drain) {
        return {.enabled = false,
                .explanation = i18n::text(i18n::TextId::ControlStopping,
                                          context.language)};
      }
      if (!lifecycle_allowed) {
        return {.enabled = false,
                .explanation = i18n::text(i18n::TextId::ControlLifecycle,
                                          context.language)};
      }
      if (!capability_available) {
        return {.enabled = false,
                .explanation = i18n::text(i18n::TextId::ControlCapability,
                                          context.language)};
      }
      if (source_missing) {
        return {.enabled = false,
                .explanation = i18n::text(i18n::TextId::ControlMissingSource,
                                          context.language)};
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

  }    // namespace

  ControllerViewState build_controller_view(
    const core::ApplicationState &state, core::SessionId current_session_id, std::span<const core::SourceDescriptor> sources, core::PlatformCapabilities capabilities, const UserConfig &config, bool hotkey_binding_confirmed, core::CaptureExclusionStatus controller_exclusion, const RuntimeFeedback &runtime_feedback, bool runtime_present, bool stop_requested, std::optional<core::AppError> application_notice, i18n::Language language, bool language_changes_allowed)
  {
    const auto &session = state.current_session();
    const auto lifecycle = session.lifecycle();
    // Error alone is insufficient: failed construction has no runtime and can
    // restart, while terminal Error with a retained runtime must finish drain.
    const bool stop_or_drain =
      stop_requested || lifecycle == core::SessionLifecycle::Stopping ||
      (runtime_present && lifecycle == core::SessionLifecycle::Error);
    const ControlContext context{.stop_or_drain = stop_or_drain,
                                 .language = language};
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
      .language_preference = config.language,
      .effective_language = language,
      .status_text = lifecycle_text(lifecycle, language),
      .settings = session.settings(),
      .hotkey_display_text = core::hotkey_display_text(config.hotkey),
      .hotkey_binding_confirmed = hotkey_binding_confirmed,
      .application_notice_text =
        application_notice.transform(
          [language](core::AppError error) {
            return error_text(error.code, language);
          }),
      .drain_status = runtime_present ? runtime_feedback.drain
                                      : core::RuntimeDrainStatus::Normal,
      .extensions_scope_text =
        i18n::text(i18n::TextId::ExtensionsScope, language),
    };

    if (lifecycle == core::SessionLifecycle::Error) {
      view.session_error_text =
        i18n::text(i18n::TextId::SessionFailed, language);
    }

    // Copy dynamic labels/identities so this snapshot never borrows source-catalog storage.
    view.sources.reserve(sources.size());
    for (const auto &source : sources) {
      const bool selected = session.selected_source() &&
                            session.selected_source()->id == source.id;
      view.sources.push_back(ControllerSourceRow{
        .id = source.id,
        .kind_glyph = source_kind_text(source.kind, language),
        .label = source.display_label,
        .selected = selected,
        .select = control(context, select_or_start_allowed,
                          supports_source(source.kind, capabilities)),
        .kind = source.kind,
        .native_handle_text = source.native_handle_text,
      });
    }

    // Static capabilities cannot predict the path chosen by actual native devices.
    // Suppress runtime-scoped feedback once that owner has been released.
    if (runtime_present && runtime_feedback.bridge) {
      view.bridge = BridgeView{
        .mode = runtime_feedback.bridge->mode,
        .reason = runtime_feedback.bridge->reason,
        .status_text = bridge_text(*runtime_feedback.bridge, language),
      };
    }

    if (session.selected_source()) {
      view.selected_source_id = session.selected_source()->id;
      view.color_warning =
        color_warning(session.selected_source()->color_support, language);
    }
    // Controller exclusion survives replica replacement; overlay exclusion and
    // stalled-drain warnings belong only to the currently retained runtime.
    view.controller_exclusion_warning = exclusion_warning(
      controller_exclusion,
      i18n::text(i18n::TextId::ControllerExclusionUnsupported, language),
      i18n::text(i18n::TextId::ControllerExclusionFailed, language));
    if (runtime_present) {
      view.overlay_exclusion_warning = exclusion_warning(
        runtime_feedback.overlay_exclusion,
        i18n::text(i18n::TextId::OverlayExclusionUnsupported, language),
        i18n::text(i18n::TextId::OverlayExclusionFailed, language));
      if (runtime_feedback.drain == core::RuntimeDrainStatus::Stalled) {
        view.drain_warning =
          i18n::text(i18n::TextId::DrainStalled, language);
      }
    }

    const bool has_source = session.selected_source().has_value();
    const bool source_capability =
      !has_source || supports_source(session.selected_source()->kind,
                                     capabilities);
    const bool region_active = runtime_present && !stop_or_drain && has_source &&
      lifecycle == core::SessionLifecycle::Capturing;
    if (region_active && runtime_feedback.content_extent &&
        runtime_feedback.content_extent->width > 0 && runtime_feedback.content_extent->height > 0) {
      view.content_extent = runtime_feedback.content_extent;
    }
    view.controls = ControllerControls{
      .refresh = control(context, inactive),
      .auto_refresh = {.enabled = language_changes_allowed},
      .start = control(context, select_or_start_allowed, source_capability,
                       !has_source),
      .stop = control(context, active),
      .editable = control(context, settings_allowed),
      .pass_through =
        control(context, settings_allowed, capabilities.pass_through),
      .fps = control(context, settings_allowed),
      .opacity = control(context, settings_allowed),
      .wheel_zoom = control(context, settings_allowed),
      .region = control(context, region_active && view.content_extent.has_value()),
      .cancel_region_selection = control(context, active && session.settings().selecting_region),
      .bordered = control(context, settings_allowed),
      .borderless =
        control(context, settings_allowed, capabilities.borderless_resize),
      .replace_hotkey =
        control(context, settings_allowed, capabilities.global_hotkey),
      .browser_capture_keep_alive = control(
        context, settings_allowed, capabilities.browser_capture_keep_alive),
      .language = {
        .enabled = language_changes_allowed,
        .explanation = language_changes_allowed
                         ? std::string_view{}
                         : i18n::text(i18n::TextId::LanguageChangesClosed,
                                      language),
      },
    };
    return view;
  }

}    // namespace avb::app
