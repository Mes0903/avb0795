/// @file ImGui-only controller widgets using immutable view data and command values.
#ifndef AVB0795_UI_CONTROLLER_VIEW_HPP
#define AVB0795_UI_CONTROLLER_VIEW_HPP

#include "avb/app/commands.hpp"
#include "avb/app/controller_view_state.hpp"

#include <functional>
#include <optional>
#include <string>

namespace avb::ui {

  /// Main-thread command delivery supplied by the host; the view never owns the queue.
  using CommandSink = std::function<void(app::AppCommand)>;

  /// Renders controls with no native window/renderer/service access.
  /// Only local text-edit state persists; authoritative settings come from each snapshot.
  class ControllerView final {
  public:
    /// Render into the host's current ImGui frame and emit requested AppCommand values.
    /// Uses the snapshot's SessionId for session actions; never mutates the snapshot.
    /// The host owns exception containment around ImGui and the potentially throwing sink.
    void render(const app::ControllerViewState &state,
                const CommandSink &command_sink);

  private:
    // Preserve unfinished input across snapshot refreshes; reset after emitting replacement.
    std::optional<std::string> hotkey_edit_text_;
  };

}    // namespace avb::ui

#endif    // AVB0795_UI_CONTROLLER_VIEW_HPP
