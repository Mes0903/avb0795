/// @file
/// @brief Language preference widgets projected from the controller snapshot.
#ifndef AVB0795_UI_LANGUAGE_SELECTOR_HPP
#define AVB0795_UI_LANGUAGE_SELECTOR_HPP

#include "avb/ui/controller_view.hpp"

namespace avb::ui {

  /// @brief Display the requested preference and emit session-agnostic language commands.
  /// System also displays its separately resolved effective language. Re-selecting
  /// a selected option may still emit, allowing the owner to refresh system policy.
  /// Availability comes directly from the snapshot and gates the whole selector.
  /// @pre An ImGui window/frame is active on the UI thread; the borrowed snapshot
  /// and sink remain valid and unchanged until return. No inputs are retained.
  void render_language_selector(const app::ControllerViewState &state,
                                const CommandSink &command_sink);

}    // namespace avb::ui

#endif    // AVB0795_UI_LANGUAGE_SELECTOR_HPP
