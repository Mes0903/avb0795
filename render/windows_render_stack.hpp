#ifndef AIRELINNA_RENDER_WINDOWS_RENDER_STACK_HPP
#define AIRELINNA_RENDER_WINDOWS_RENDER_STACK_HPP

#include "airelinna/core/types.hpp"
#include "render/bridge_policy.hpp"
#include "render/graphics_retry_policy.hpp"

#include <dxgiformat.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

struct ID3D11Device;

namespace airelinna::platform::windows {
  class OverlayWindow;
  class WindowsCapturedFrame;
}    // namespace airelinna::platform::windows

namespace airelinna::render {

  enum class WindowsBridgePreference { Automatic,
                                       ForceCpuBaseline };

  struct RenderTickResult final {
    bool surface_recovery_started{};
    bool surface_recovery_completed{};
    bool vulkan_device_lost{};
    bool d3_device_removed{};
    std::optional<DeviceLossKind> device_loss;
    std::optional<core::AppError> terminal_error;
    bool made_progress{};
    bool drain_complete{};
    bool cleanup_pending{};
    bool presentation_suspended{};
    std::uint64_t captured_frames{};
    std::uint64_t presented_frames{};
    std::uint64_t dropped_frames{};
    std::size_t slots_in_flight{};
    std::size_t presentations_in_flight{};
    std::optional<std::chrono::steady_clock::time_point> next_deadline;
  };

  class WindowsRenderStack final {
  public:
    // Construction throws core::AppError so an unsupported mandatory graphics
    // feature reaches the Windows factory as a typed construction failure.
    WindowsRenderStack(platform::windows::OverlayWindow &overlay,
                       std::uint64_t resource_generation,
                       WindowsBridgePreference bridge_preference =
                         WindowsBridgePreference::Automatic);
    ~WindowsRenderStack() noexcept;

    WindowsRenderStack(const WindowsRenderStack &) = delete;
    WindowsRenderStack &operator=(const WindowsRenderStack &) = delete;
    WindowsRenderStack(WindowsRenderStack &&) = delete;
    WindowsRenderStack &operator=(WindowsRenderStack &&) = delete;

    [[nodiscard]] ID3D11Device &capture_device() noexcept;
    [[nodiscard]] bool ready_for_frame() const noexcept;
    void submit(platform::windows::WindowsCapturedFrame &&frame);
    [[nodiscard]] RenderTickResult
    tick(core::Extent framebuffer,
         std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool render_allocation_idle() const noexcept;
    [[nodiscard]] std::expected<void, core::AppError>
    rebuild_capture_slots(std::uint64_t generation, core::Extent allocation, DXGI_FORMAT format);
    void resize(core::Extent framebuffer) noexcept;
    void begin_drain() noexcept;
    void begin_device_recovery_drain(DeviceLossKind kind) noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] BridgeCapabilityFacts bridge_capability_facts() const noexcept;
    [[nodiscard]] std::optional<BridgeSelection>
    bridge_selection() const noexcept;
    [[nodiscard]] core::BridgeMode active_bridge_mode() const noexcept;
    [[nodiscard]] std::uint64_t gpu_interop_frames_submitted() const noexcept;
    [[nodiscard]] std::optional<BridgeSelection>
    take_bridge_mode_change() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
  };

}    // namespace airelinna::render

#endif    // AIRELINNA_RENDER_WINDOWS_RENDER_STACK_HPP
