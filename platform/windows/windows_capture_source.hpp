/// @file
/// @brief Stable registry-item lease and publication-gated runtime wake binding.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_CAPTURE_SOURCE_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_CAPTURE_SOURCE_HPP

#include "platform/windows/windows_source_registry.hpp"

#include <unknwn.h>
#include <winrt/Windows.Graphics.Capture.h>

#include <expected>
#include <functional>
#include <memory>

namespace avb::platform::windows {

  class WgcCaptureJob;

  /// @brief Preserve selected source identity across job rebuilds and recovery.
  ///
  /// The factory transfers one registry lease here; rebuilding graphics resources
  /// never repeats selection or recreates the native item from a window handle.
  /// Main-thread operations consume typed close state. The wake callable merely
  /// requests a tick and may borrow a host whose lifetime must cover admitted
  /// calls, including the final wake issued as callback admission drains.
  class WindowsCaptureSource final {
  public:
    using WakeMainLoop = std::function<void()>;

    /// @brief Consume a source lease with runtime wake admission still unpublished.
    ///
    /// The observation continues recording closure before activation, so a source
    /// lost during construction is detectable without exposing the borrowed host.
    /// An empty wake callable or construction failure returns a typed error.
    [[nodiscard]] static std::expected<std::unique_ptr<WindowsCaptureSource>,
                                       core::AppError>
    create(WindowsSourceLease lease, WakeMainLoop wake_main_loop);

    ~WindowsCaptureSource();

    WindowsCaptureSource(const WindowsCaptureSource &) = delete;
    WindowsCaptureSource &operator=(const WindowsCaptureSource &) = delete;
    WindowsCaptureSource(WindowsCaptureSource &&) = delete;
    WindowsCaptureSource &operator=(WindowsCaptureSource &&) = delete;

    // The P3 runtime drains this typed edge/state pair on its main-thread tick.
    // Neither operation exposes the GraphicsCaptureItem or callback machinery.
    /// Consume this source's close cursor; the underlying sticky fact remains.
    [[nodiscard]] bool consume_closed_edge() noexcept;
    /// Observe closure even before runtime activation or between job generations.
    [[nodiscard]] bool is_closed() const noexcept;

    /// @brief Bind the preallocated wake after runtime publication is guaranteed.
    ///
    /// The factory calls this after successful capture start; an unpublished
    /// failure can therefore unwind without a callback borrowing the host.
    /// Activation after stop is ignored. Sticky closure must still be checked
    /// independently because a close recorded before binding need not wake again.
    void activate_runtime_wake() noexcept;

    /// @brief Close wake admission and detach this source's runtime binding.
    ///
    /// Already admitted calls finish independently; retain this object and the
    /// borrowed wake target until callbacks_quiescent() becomes true. Stop leaves
    /// the registry's longer-lived close observation intact for future selection.
    void request_stop() noexcept;
    /// True only after all admitted wakes, including their final wake, return.
    [[nodiscard]] bool callbacks_quiescent() const noexcept;

  private:
    class Impl;
    explicit WindowsCaptureSource(std::unique_ptr<Impl> implementation) noexcept;

    friend class WgcCaptureJob;
    [[nodiscard]] const winrt::Windows::Graphics::Capture::GraphicsCaptureItem &
    item() const noexcept;

    std::unique_ptr<Impl> implementation_;
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_CAPTURE_SOURCE_HPP
