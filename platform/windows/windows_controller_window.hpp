/// @file
/// @brief Borrow a controller window while owning its native interaction callback.
#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_CONTROLLER_WINDOW_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_CONTROLLER_WINDOW_HPP

#include "avb/core/types.hpp"
#include "avb/desktop/controller_window_interaction.hpp"

#include <windows.h>

#include <expected>
#include <memory>

namespace avb::platform::windows {

  /// @brief Owner-thread subclass for full-client controller drag and resize.
  /// The native handle is borrowed; the stable callback receiver cannot move.
  /// Destroy after upstream input callbacks have shut down and before the native
  /// window is released. Native destruction also detaches the receiver. Wrong-
  /// thread destruction or an unremovable live callback terminates rather than
  /// leaving the system with a pointer to freed storage.
  class WindowsControllerWindow final : public desktop::ControllerWindowInteraction {
  public:
    /// @brief Attach only to a valid window owned by this process and thread.
    /// Saves the original style and restores it if setup fails. Duplicate
    /// attachment is rejected. Native failures carry a typed error; allocation
    /// may throw before native state changes. The window remains caller-owned.
    [[nodiscard]] static std::expected<std::unique_ptr<WindowsControllerWindow>, core::AppError>
    create(HWND window);
    ~WindowsControllerWindow() override;

    WindowsControllerWindow(const WindowsControllerWindow &) = delete;
    WindowsControllerWindow &operator=(const WindowsControllerWindow &) = delete;
    WindowsControllerWindow(WindowsControllerWindow &&) = delete;
    WindowsControllerWindow &operator=(WindowsControllerWindow &&) = delete;

    /// Clamp finite client fractions; nonfinite values disable the title region.
    /// Off-owner calls are ignored. Resize edges remain active with no title.
    void set_drag_region(float right_fraction, float bottom_fraction) noexcept override;

  private:
    explicit WindowsControllerWindow(DWORD owner_thread_id) noexcept;
    void detach() noexcept;
    [[nodiscard]] LRESULT hit_test(HWND window, LPARAM coordinates) const noexcept;
    static LRESULT CALLBACK subclass_proc(HWND window, UINT message,
                                          WPARAM word_parameter, LPARAM long_parameter,
                                          UINT_PTR subclass_id, DWORD_PTR receiver) noexcept;

    HWND window_{};
    DWORD owner_thread_id_{};
    float drag_right_{};
    float drag_bottom_{};
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_CONTROLLER_WINDOW_HPP
