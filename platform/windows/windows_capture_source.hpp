/// @file
/// @brief Stable registry-item lease and publication-gated runtime wake binding.
/// Native implementation details remain below the portable application ports.

#ifndef AIRELINNA_PLATFORM_WINDOWS_WINDOWS_CAPTURE_SOURCE_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WINDOWS_CAPTURE_SOURCE_HPP

#include "platform/windows/windows_source_registry.hpp"

#include <unknwn.h>
#include <winrt/Windows.Graphics.Capture.h>

#include <expected>
#include <functional>
#include <memory>

namespace airelinna::platform::windows {

  class WgcCaptureJob;

  // Stable P2/P3 source handoff. The P3 runtime factory resolves a catalog entry
  // once, transfers its WindowsSourceLease here, and keeps this object alive
  // across every capture-job/resource generation. The wake callable only asks
  // the owning main loop to tick; source state remains local and is consumed
  // synchronously through the operations below.
  /// Stable source identity across capture-job rebuilds and graphics recovery.
  /// Main-thread operations retain a preallocated wake state for callback drains.
  class WindowsCaptureSource final {
  public:
    using WakeMainLoop = std::function<void()>;

    /// Consume a registry lease without activating borrowed host callbacks.
    /// Unpublished construction failures may therefore unwind immediately.
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

    // Bind the preallocated runtime wake only after successful publication.
    // Before activation, the registry observation still records sticky close.
    /// Enable callback wake access only when successful StartCapture guarantees publication.
    void activate_runtime_wake() noexcept;

    // Stop detaches the runtime wake binding without waiting or retiring the
    // registry-owned source observation. Retain this object until callbacks are
    // quiescent.
    /// Close admission and detach the binding without waiting for admitted calls.
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

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_CAPTURE_SOURCE_HPP
