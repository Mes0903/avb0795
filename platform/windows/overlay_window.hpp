/// @file
/// @brief Main-thread GLFW overlay ownership and decoration changes.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_OVERLAY_WINDOW_HPP
#define AVB0795_PLATFORM_WINDOWS_OVERLAY_WINDOW_HPP

#include "avb/core/types.hpp"

#include <expected>
#include <memory>

struct GLFWwindow;

namespace avb::render {
  class WindowsRenderStack;
}

namespace avb::platform::windows {

  class Win32OverlayAdapter;
  class WindowsRuntimeFactory;

  /// @brief Own the floating replica window used by native input and presentation.
  ///
  /// All operations and destruction belong to the GLFW main thread while GLFW
  /// remains initialized. The wrapper controls decoration; a separate adapter
  /// owns subclass and input-transparency policy. Render surfaces and subclass
  /// reachability must be retired before releasing the window they borrow.
  class OverlayWindow final {
  private:
    struct Impl;
    struct ConstructionToken final {
    private:
      ConstructionToken() = default;
      friend class OverlayWindow;
    };

  public:
    /// Create a Vulkan-compatible floating window with the requested border.
    [[nodiscard]] static std::expected<std::unique_ptr<OverlayWindow>,
                                       core::AppError>
    create(core::Extent initial_extent, core::BorderPreference border);

    OverlayWindow(ConstructionToken,
                  std::unique_ptr<Impl> implementation) noexcept;
    ~OverlayWindow() noexcept;

    OverlayWindow(const OverlayWindow &) = delete;
    OverlayWindow &operator=(const OverlayWindow &) = delete;
    OverlayWindow(OverlayWindow &&) = delete;
    OverlayWindow &operator=(OverlayWindow &&) = delete;

    /// @brief Change decoration and cache the preference only after native readback.
    ///
    /// This operation does not preserve client placement by itself. The caller
    /// snapshots and restores geometry as part of the surrounding settings
    /// transaction. An error can follow a native mutation, so it does not prove
    /// the old border is still applied or remove the need for rollback.
    [[nodiscard]] std::expected<void, core::AppError>
    set_border(core::BorderPreference border) noexcept;
    /// Return the owner's confirmed border preference.
    [[nodiscard]] core::BorderPreference border() const noexcept;
    /// Verify the actual GLFW attribute rather than trusting only cached settings.
    [[nodiscard]] bool
    border_matches(core::BorderPreference border) const noexcept;

    /// Observe native user close intent without destroying the runtime-owned window.
    [[nodiscard]] bool close_requested() const noexcept;
    /// Update close intent while shutdown remains controlled by the runtime.
    void set_close_requested(bool requested) noexcept;
    /// Return physical framebuffer pixels; zero extent suspends presentation.
    [[nodiscard]] core::Extent framebuffer_extent() const noexcept;

    /// @brief Destroy the window after its native users have safely retired.
    ///
    /// Explicit destruction clears the stored pointer so later destruction is
    /// harmless. If subclass detach failed, keep the adapter alive through native
    /// destruction so the final messages still reach valid callback state.
    /// @pre All presentation and surface use of the window has ended.
    void destroy() noexcept;

  private:
    friend class Win32OverlayAdapter;
    friend class WindowsRuntimeFactory;
    friend class avb::render::WindowsRenderStack;
    [[nodiscard]] GLFWwindow *native_window() const noexcept;

    std::unique_ptr<Impl> implementation_;
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_OVERLAY_WINDOW_HPP
