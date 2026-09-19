/// @file
/// @brief ImGui-only controller widgets using immutable view data and command values.
#ifndef AVB0795_UI_CONTROLLER_VIEW_HPP
#define AVB0795_UI_CONTROLLER_VIEW_HPP

#include "avb/app/commands.hpp"
#include "avb/app/controller_view_state.hpp"

#include <functional>
#include <array>
#include <optional>
#include <string>

namespace avb::ui {

  /// @brief Main-thread delivery of owning command values through host-supplied policy.
  /// Widgets call the sink synchronously and retain neither it nor its captures.
  /// The host normally queues values for a later step; delivery is not proof that
  /// a requested change was accepted. An empty sink simply suppresses emission.
  using CommandSink = std::function<void(app::AppCommand)>;

  /// @brief Presentation facts for the host's controller-window interaction.
  /// The drag rectangle starts at the client origin and ends at drag_right /
  /// drag_bottom in ImGui display units. Zero bounds reserve all client input
  /// for widgets or a popup. Minimize is a one-frame request; close continues
  /// through the existing application command sink and orderly shutdown path.
  struct ControllerFrame {
    float drag_right{};
    float drag_bottom{};
    bool minimize_requested{};
  };

  /// @brief Render an immutable controller projection and emit requested command values.
  /// Local recording state persists; authoritative settings come from each snapshot.
  /// Catalog labels may change language without changing widget identities, while
  /// dynamic source labels remain literal text independent of their interaction IDs.
  class ControllerView final {
  public:
    /// @brief Render into the host's active ImGui frame using borrowed snapshot/sink values.
    /// Session actions copy this snapshot's SessionId; emitted commands own their
    /// payloads. The view retains no input reference after return, and only a later
    /// projected snapshot confirms whether a command took effect.
    /// @pre Call on the UI thread with an active ImGui frame. The sink must not
    /// invalidate or mutate the supplied snapshot during rendering or reenter this
    /// view. The host owns exception containment for ImGui and the potentially
    /// throwing sink; this method does not restore every ImGui stack on exception.
    /// The host supplies resize_margin in display units to reserve the native
    /// edge hit band at the current display scale. The root follows the main
    /// viewport, while a child region scrolls the controls below a fixed title.
    ControllerFrame render(const app::ControllerViewState &state,
                           const CommandSink &command_sink,
                           float resize_margin = 8.0F);

    /// @brief Offer a confirmed global chord to an active, focused recorder.
    /// The host checks native focus and shutdown at event time before calling.
    /// An active recorder consumes the event, including its initial release wait,
    /// so the existing binding can be recorded without executing its recovery action.
    /// This context-free value operation leaves OS registration to the orchestrator.
    [[nodiscard]] bool try_record_hotkey(core::HotkeyChord chord) noexcept;
    /// Discard the candidate immediately; the next render closes its modal.
    void cancel_hotkey_recording() noexcept;

    /// @brief Report an ongoing local gesture that benefits from interactive cadence.
    /// Recorder release barriers and numeric drafts persist between frames. This
    /// context-free observation lets the host retain responsive frame timing while
    /// the view continues to own those transient interaction values.
    [[nodiscard]] bool has_active_interaction() const noexcept
    {
      return hotkey_recording_.has_value() || opacity_editing_.has_value() ||
             wheel_zoom_editing_.has_value();
    }

  private:
    void render_contents(const app::ControllerViewState &state,
                         const CommandSink &command_sink);
    void render_hotkey_recorder(const app::ControllerViewState &state,
                                const CommandSink &command_sink);
    void render_region(const app::ControllerViewState &state,
                       const CommandSink &command_sink);
    /// @brief Collect one wheel-step gesture and submit its completed value.
    /// The UI owns the temporary percentage; the orchestrator owns admission,
    /// runtime application and preference persistence. Widget identity includes
    /// the session and availability so replacement snapshots retire old input.
    void render_wheel_zoom(const app::ControllerViewState &state,
                           const CommandSink &command_sink);
    /// Short-lived UI state tied to the snapshot that opened the modal.
    struct HotkeyRecording {
      core::SessionId session_id;
      std::optional<core::HotkeyChord> candidate;
      bool waiting_for_release{true};
      bool open_requested{true};
    };
    std::optional<HotkeyRecording> hotkey_recording_;
    /// Keep a gesture's draft and token stable until release, even if snapshots change.
    struct OpacityEditing {
      core::SessionId session_id;
      int percent{100};
      // Persist the end-of-gesture obligation across hidden input-only frames,
      // which can outlive ImGui's per-frame deactivation notification.
      bool preview_emitted{};
    };
    std::optional<OpacityEditing> opacity_editing_;
    /// @brief Retain a wheel-step edit through visible and hidden input frames.
    /// The originating token binds the draft to one session. `changed` records
    /// the pending delivery obligation across hidden frames that consume ImGui's
    /// deactivation notification. Release submits one value command; session or
    /// availability changes discard the draft before the next widget is rendered.
    struct WheelZoomEditing {
      core::SessionId session_id;
      int percent{core::default_wheel_zoom_percent};
      bool changed{};
    };
    std::optional<WheelZoomEditing> wheel_zoom_editing_;
    /// Owned coordinate draft, discarded when any source/presentation identity changes.
    struct RegionDraft {
      core::SessionId session_id;
      core::SourceId source_id;
      core::Extent extent;
      std::optional<core::PixelRegion> accepted_region;
      std::array<int, 4> coordinates{};
      std::string widget_scope;
    };
    std::optional<RegionDraft> region_draft_;
  };

}    // namespace avb::ui

#endif    // AVB0795_UI_CONTROLLER_VIEW_HPP
