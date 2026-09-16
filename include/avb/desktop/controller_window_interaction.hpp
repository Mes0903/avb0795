/// @file
/// @brief Portable, owner-thread interaction contract for the controller window.
#ifndef AVB0795_DESKTOP_CONTROLLER_WINDOW_INTERACTION_HPP
#define AVB0795_DESKTOP_CONTROLLER_WINDOW_INTERACTION_HPP

namespace avb::desktop {

  /// @brief Supply a client-relative title region without exposing native handles.
  ///
  /// Calls and destruction belong to the window-owning thread. The interaction
  /// owner must be released before its borrowed native window is destroyed.
  class ControllerWindowInteraction {
  public:
    virtual ~ControllerWindowInteraction() = default;

    /// @brief Set the top-left anchored title rectangle as client-size fractions.
    /// Right and bottom are exclusive; zero disables caption dragging. Resize
    /// edges take precedence. Nonfinite geometry disables caption dragging;
    /// finite fractions are clamped to [0, 1]. Publish after layout each frame.
    virtual void set_drag_region(float right_fraction,
                                 float bottom_fraction) noexcept = 0;

    ControllerWindowInteraction(const ControllerWindowInteraction &) = delete;
    ControllerWindowInteraction &operator=(const ControllerWindowInteraction &) = delete;
    ControllerWindowInteraction(ControllerWindowInteraction &&) = delete;
    ControllerWindowInteraction &operator=(ControllerWindowInteraction &&) = delete;

  protected:
    ControllerWindowInteraction() = default;
  };

}    // namespace avb::desktop

#endif    // AVB0795_DESKTOP_CONTROLLER_WINDOW_INTERACTION_HPP
