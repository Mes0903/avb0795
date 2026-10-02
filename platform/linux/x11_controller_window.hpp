/// @file
/// @brief Borrowed controller interaction for the GLFW X11 backend.
#ifndef AVB0795_LINUX_X11_CONTROLLER_WINDOW_HPP
#define AVB0795_LINUX_X11_CONTROLLER_WINDOW_HPP
#include "avb/core/types.hpp"
#include "avb/desktop/controller_window_interaction.hpp"
#include <expected>
#include <memory>
#include <string>
struct GLFWwindow;
namespace avb::linux_platform {
  /// @brief Attach caption/grip callbacks without taking ownership or using the host user pointer.
  /// Creation, calls and destruction belong to the initialized GLFW main thread.
  /// Release after host callback-chain teardown and before destroying the borrowed
  /// window. Eight logical pixel grips override the exclusive title rectangle.
  class X11ControllerWindow final : public desktop::ControllerWindowInteraction {
  public:
    static std::expected<std::unique_ptr<desktop::ControllerWindowInteraction>, core::AppError> create(GLFWwindow *);
    ~X11ControllerWindow() override;
    void set_drag_region(float, float) noexcept override;

  private:
    struct Impl;
    explicit X11ControllerWindow(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
  };
  /// @brief Format only the caller-owned initialized X11 window for a Portal parent.
  [[nodiscard]] std::expected<std::string, core::AppError> portal_parent_identifier(GLFWwindow *);
  namespace detail {
    /// @brief Native WM capability and gesture operations shared only by Linux adapters.
    /// No GLFW event dispatch occurs inside their narrow protocol error scopes.
    [[nodiscard]] bool interactive_supported(GLFWwindow *) noexcept;
    [[nodiscard]] std::expected<void, core::AppError> moveresize(GLFWwindow *, int, double, double) noexcept;
  }    // namespace detail
}    // namespace avb::linux_platform
#endif
