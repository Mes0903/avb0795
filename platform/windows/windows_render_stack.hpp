/// @file
/// @brief Concrete replica graphics boundary: the runtime supplies capture checkouts
/// and receives native facts/deadlines, without exposing graphics to the UI.
#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_RENDER_STACK_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_RENDER_STACK_HPP

#include "avb/core/types.hpp"
#include "platform/windows/detail/bridge_policy.hpp"
#include "platform/windows/detail/windows_graphics_policy.hpp"
#include "render/graphics_retry_policy.hpp"
#include "render/render_math.hpp"
#include "render/replica_presentation.hpp"

#include <dxgiformat.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

struct ID3D11Device;

namespace avb::diagnostics { class PerformanceReport; }

namespace avb::platform::windows {
  class OverlayWindow;
  class WindowsCapturedFrame;
}    // namespace avb::platform::windows

namespace avb::render {

  /// @brief Value snapshot of one main-thread progress pass.
  ///
  /// Device-loss flags are independent; drain_complete is release evidence,
  /// whereas cleanup_pending and deadlines describe work still needing ticks.
  /// Counters are cumulative for this stack. presented_frames advances when a
  /// successful/suboptimal present fence retires, not when submission succeeds.
  /// device_loss selects Vulkan loss first if both flags are set; inspect both
  /// flags when deciding which independent cleanup obligations remain.
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
    std::optional<PresentationSnapshot> presentation;
  };

  /// @brief Compose the Windows frame bridge and shared Vulkan renderer for one replica.
  ///
  /// The bridge owns native copies/imports and the renderer owns canonical slots,
  /// descriptors, submissions and WSI. This facade selects transport and combines
  /// independent progress/loss facts. All calls run on the main thread.
  /// The borrowed OverlayWindow must outlive
  /// this stack; published owners must drain before destruction.
  /// Copy and move are disabled so outstanding work keeps a stable native owner.
  class WindowsRenderStack final {
  public:
    /// @brief Create graphics against an existing overlay and nonzero generation.
    ///
    /// Borrows the overlay for the stack lifetime. Capture slots are prepared
    /// separately before capture starts; a zero framebuffer defers WSI creation.
    /// Throws core::AppError for invalid inputs or native setup failure; C++
    /// allocation failures can also propagate from construction.
    /// An enabled collector outlives this stack and every replacement using its
    /// observation instance. Instance identifies a runtime across device rebuilds;
    /// resource_generation describes the runtime's current allocation epoch.
    WindowsRenderStack(platform::windows::OverlayWindow &overlay,
                       std::uint64_t resource_generation,
                       WindowsBridgePreference bridge_preference =
                         WindowsBridgePreference::Automatic,
                       diagnostics::PerformanceReport *performance = nullptr,
                       std::uint64_t observation_instance = 0);
    /// @brief Release resources after native-use or matching device-loss proof.
    ///
    /// The caller must finish the drain protocol before destroying a published
    /// stack. Outstanding healthy-device work causes std::terminate(); destruction
    /// neither waits for fences nor turns a terminal error into completion proof.
    ~WindowsRenderStack() noexcept;

    WindowsRenderStack(const WindowsRenderStack &) = delete;
    WindowsRenderStack &operator=(const WindowsRenderStack &) = delete;
    WindowsRenderStack(WindowsRenderStack &&) = delete;
    WindowsRenderStack &operator=(WindowsRenderStack &&) = delete;

    /// @brief Borrow the device used to construct this generation's WGC frame pool.
    ///
    /// The reference remains tied to this stack's lifetime and transfers no COM
    /// reference. The caller must retire capture users before replacing the stack.
    [[nodiscard]] ID3D11Device &capture_device() noexcept;
    /// @brief Report copy admission and empty-slot capacity without consuming a frame.
    ///
    /// This is an observation, not a reservation or a promise that WSI can present.
    [[nodiscard]] bool ready_for_frame() const noexcept;
    /// @brief Consume a matching checkout into an available D3 source-copy slot.
    ///
    /// Requires ready_for_frame(), this generation's allocation and BGRA format,
    /// positive in-bounds content, and a texture from capture_device(). Validation
    /// failure throws core::AppError before adoption. Once adopted, the slot keeps
    /// the WGC checkout until copy completion or D3 removal is proven. Returning
    /// does not promise presentation: the frame can be dropped during recovery.
    void submit(platform::windows::WindowsCapturedFrame &&frame);
    /// @brief Coalesce a presentation edit into the next admitted draw.
    /// Stores an owned value and marks retained content dirty. Capture allocation,
    /// native window styles and scheduling remain with their existing owners.
    void set_presentation(ReplicaPresentation presentation) noexcept;
    /// Supply the runtime-owned scheduler setting as observation metadata. This
    /// value describes workload; frame admission remains with the runtime.
    void set_performance_fps(core::FpsCap fps) noexcept;
    /// @brief Report a dirty retained image eligible for scheduler admission.
    [[nodiscard]] bool has_redraw_work() const noexcept;
    /// @brief Grant one redraw attempt after the runtime's FPS admission.
    /// Completion and recovery polling continue independently of this permission.
    void request_redraw() noexcept;
    /// @brief Poll completion and advance presentation/recovery on the main thread.
    ///
    /// Supply physical framebuffer pixels and the current steady-clock time.
    /// Continue calling after admission closes or while the framebuffer is zero:
    /// completion still progresses. Native failures become result facts/errors;
    /// pending work yields a future deadline without a synchronous fence wait.
    [[nodiscard]] RenderTickResult
    tick(core::Extent framebuffer,
         std::chrono::steady_clock::time_point now) noexcept;
    /// @brief Test whether the capture slot allocations have no native users.
    ///
    /// Ready or retained content may still exist and can be discarded by rebuild.
    /// Independent present records may remain alive; this is not whole-stack drain
    /// completion or evidence that capture callbacks/checkouts are quiescent.
    [[nodiscard]] bool render_allocation_idle() const noexcept;
    /// @brief Replace the bounded ring after capture and old-allocation barriers.
    ///
    /// The caller closes capture admission, retires callbacks/checkouts, and checks
    /// render_allocation_idle(). Supply a nonzero generation, nonzero representable
    /// allocation, and DXGI_FORMAT_B8G8R8A8_UNORM on healthy devices. Invalid inputs
    /// or native construction failures return unexpected. Once rebuilding starts,
    /// failure need not preserve the old ring; callers must enter safe recovery.
    /// generation() changes only after all replacement slots are constructed.
    [[nodiscard]] std::expected<void, core::AppError>
    rebuild_capture_slots(std::uint64_t generation, core::Extent allocation, DXGI_FORMAT format);
    /// @brief Record physical framebuffer dimensions for subsequent ticks.
    ///
    /// Nonzero changes request WSI recreation; zero suspends presentation without
    /// ending completion polling or changing capture allocation/generation.
    void resize(core::Extent framebuffer) noexcept;
    /// @brief Close copy admission and discard unsubmitted or retained redraw content.
    ///
    /// Repeated calls are allowed. Later ticks must retire outstanding D3, acquire,
    /// submission, and presentation work before drain_complete permits teardown.
    void begin_drain() noexcept;
    /// @brief Add a confirmed device-loss fact and apply only its legal cleanup rule.
    ///
    /// Vulkan loss does not prove D3 copies complete, nor does D3 removal prove
    /// healthy Vulkan work complete.
    void begin_device_recovery_drain(DeviceLossKind kind) noexcept;
    /// @brief Return the resource generation supplied by the owning runtime.
    [[nodiscard]] std::uint64_t generation() const noexcept;
    /// @brief Snapshot the concrete capability probes, without querying the driver.
    [[nodiscard]] BridgeCapabilityFacts bridge_capability_facts() const noexcept;
    /// @brief Return the published bridge choice once slots have been prepared.
    [[nodiscard]] std::optional<BridgeSelection>
    bridge_selection() const noexcept;
    /// @brief Report the transport currently implemented by the owned slot ring.
    [[nodiscard]] core::BridgeMode active_bridge_mode() const noexcept;
    /// @brief Count successfully enqueued GPU-interop submissions, including redraws.
    [[nodiscard]] std::uint64_t gpu_interop_frames_submitted() const noexcept;
    /// @brief Consume the pending bridge change once for runtime event projection.
    ///
    /// Returns an independent value and clears the pending edge; another call is
    /// empty until a later selection change. This is a single pending value, not
    /// an event queue, so intermediate changes may be superseded.
    [[nodiscard]] std::optional<BridgeSelection>
    take_bridge_mode_change() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
  };

}    // namespace avb::render

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_RENDER_STACK_HPP
