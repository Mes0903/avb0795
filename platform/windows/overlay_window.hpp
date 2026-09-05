#ifndef AIRELINNA_PLATFORM_WINDOWS_OVERLAY_WINDOW_HPP
#define AIRELINNA_PLATFORM_WINDOWS_OVERLAY_WINDOW_HPP

#include "airelinna/core/types.hpp"

#include <expected>
#include <memory>

struct GLFWwindow;

namespace airelinna::render {
class WindowsRenderStack;
}

namespace airelinna::platform::windows {

class Win32OverlayAdapter;
class WindowsRuntimeFactory;

class OverlayWindow final {
private:
  struct Impl;
  struct ConstructionToken final {
  private:
    ConstructionToken() = default;
    friend class OverlayWindow;
  };

public:
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

  [[nodiscard]] std::expected<void, core::AppError>
  set_border(core::BorderPreference border) noexcept;
  [[nodiscard]] core::BorderPreference border() const noexcept;
  [[nodiscard]] bool
  border_matches(core::BorderPreference border) const noexcept;

  [[nodiscard]] bool close_requested() const noexcept;
  void set_close_requested(bool requested) noexcept;
  [[nodiscard]] core::Extent framebuffer_extent() const noexcept;

  void destroy() noexcept;

private:
  friend class Win32OverlayAdapter;
  friend class WindowsRuntimeFactory;
  friend class airelinna::render::WindowsRenderStack;
  [[nodiscard]] GLFWwindow *native_window() const noexcept;

  std::unique_ptr<Impl> implementation_;
};

} // namespace airelinna::platform::windows

#endif // AIRELINNA_PLATFORM_WINDOWS_OVERLAY_WINDOW_HPP
