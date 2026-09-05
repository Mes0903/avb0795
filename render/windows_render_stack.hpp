/// @file
/// Concrete replica graphics boundary: the runtime supplies capture checkouts
/// and receives native facts/deadlines, without exposing graphics to the UI.
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

  /// Allocation-free snapshot of one main-thread progress pass.
  /// Device-loss flags are independent; drain_complete is release evidence,
  /// whereas cleanup_pending and deadlines describe work still needing ticks.
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

  /// Own one replica's D3D device, Vulkan device/WSI, and three capture slots.
  /// All calls run on the main thread. The borrowed OverlayWindow must outlive
  /// this stack; published owners must drain before destruction.
  /// Controller rendering has a separate device and owner in DesktopHost.
  class WindowsRenderStack final {
  public:
    /// Create graphics against an existing overlay and runtime-owned generation.
    /// Throws core::AppError for unsupported required features or native setup
    /// failure; capture slots are prepared separately before capture starts.
    WindowsRenderStack(platform::windows::OverlayWindow &overlay,
                       std::uint64_t resource_generation,
                       WindowsBridgePreference bridge_preference =
                         WindowsBridgePreference::Automatic);
    /// Release only proven-safe owners; unsafe healthy-device work terminates
    /// rather than being destroyed or synchronously waited upon.
    ~WindowsRenderStack() noexcept;

    WindowsRenderStack(const WindowsRenderStack &) = delete;
    WindowsRenderStack &operator=(const WindowsRenderStack &) = delete;
    WindowsRenderStack(WindowsRenderStack &&) = delete;
    WindowsRenderStack &operator=(WindowsRenderStack &&) = delete;

    /// Borrow the device used to construct this generation's WGC frame pool.
    [[nodiscard]] ID3D11Device &capture_device() noexcept;
    /// Report admission and empty-slot availability without consuming a frame.
    [[nodiscard]] bool ready_for_frame() const noexcept;
    /// Move a matching checkout into an available slot and enqueue its D3 copy.
    /// The slot retains the WGC checkout until copy completion/removal is proven.
    /// Requires ready_for_frame(); invalid generation/metadata throws AppError.
    void submit(platform::windows::WindowsCapturedFrame &&frame);
    /// Poll native completion, advance presentation/recovery, and return the
    /// next deadline. Never waits for a fence; call even without a new frame.
    [[nodiscard]] RenderTickResult
    tick(core::Extent framebuffer,
         std::chrono::steady_clock::time_point now) noexcept;
    /// Prove old capture allocation users are retired. Unsubmitted ready images
    /// may still exist and can be discarded by rebuild_capture_slots().
    [[nodiscard]] bool render_allocation_idle() const noexcept;
    /// Replace the bounded slot ring after callback/check-out and old-allocation
    /// barriers. The runtime supplies the next generation and actual pool extent.
    [[nodiscard]] std::expected<void, core::AppError>
    rebuild_capture_slots(std::uint64_t generation, core::Extent allocation, DXGI_FORMAT format);
    /// Record framebuffer dimensions; nonzero changes request WSI recreation.
    void resize(core::Extent framebuffer) noexcept;
    /// Close new-frame admission and discard unsubmitted ready content; later
    /// ticks retain and retire all outstanding D3/Vulkan/presentation work.
    void begin_drain() noexcept;
    /// Add a confirmed device-loss fact and apply only its legal cleanup rule.
    /// Vulkan loss does not prove D3 copies complete, nor does D3 removal prove
    /// healthy Vulkan work complete.
    void begin_device_recovery_drain(DeviceLossKind kind) noexcept;
    /// Return the resource generation supplied by the owning runtime.
    [[nodiscard]] std::uint64_t generation() const noexcept;
    /// Snapshot the concrete capability probes, without querying the driver.
    [[nodiscard]] BridgeCapabilityFacts bridge_capability_facts() const noexcept;
    /// Return the published bridge choice once slots have been prepared.
    [[nodiscard]] std::optional<BridgeSelection>
    bridge_selection() const noexcept;
    /// Report the transport currently implemented by the owned slot ring.
    [[nodiscard]] core::BridgeMode active_bridge_mode() const noexcept;
    /// Diagnostic count of successfully enqueued GPU-interop submissions.
    [[nodiscard]] std::uint64_t gpu_interop_frames_submitted() const noexcept;
    /// Consume the pending bridge change once for runtime event projection.
    [[nodiscard]] std::optional<BridgeSelection>
    take_bridge_mode_change() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
  };

}    // namespace airelinna::render

#endif    // AIRELINNA_RENDER_WINDOWS_RENDER_STACK_HPP
