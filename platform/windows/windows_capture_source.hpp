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
class WindowsCaptureSource final {
public:
  using WakeMainLoop = std::function<void()>;

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
  [[nodiscard]] bool consume_closed_edge() noexcept;
  [[nodiscard]] bool is_closed() const noexcept;

  // Stop detaches the runtime wake binding without waiting or retiring the
  // registry-owned source observation. Retain this object until callbacks are
  // quiescent.
  void request_stop() noexcept;
  [[nodiscard]] bool callbacks_quiescent() const noexcept;

private:
  class Impl;
  explicit WindowsCaptureSource(std::unique_ptr<Impl> implementation) noexcept;

  friend class WgcCaptureJob;
  [[nodiscard]] const winrt::Windows::Graphics::Capture::GraphicsCaptureItem &
  item() const noexcept;

  std::unique_ptr<Impl> implementation_;
};

} // namespace airelinna::platform::windows

#endif // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_CAPTURE_SOURCE_HPP
