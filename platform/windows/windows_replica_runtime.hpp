/// @file
/// @brief Main-thread replica aggregate, native progress, and ordered safe teardown.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_REPLICA_RUNTIME_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_REPLICA_RUNTIME_HPP

#include "avb/app/ports.hpp"
#include "avb/core/frame_scheduler.hpp"
#include "platform/windows/detail/windows_runtime_policy.hpp"
#include "render/graphics_retry_policy.hpp"
#include "render/windows_render_stack.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace avb::platform::windows {

  namespace features {
    class BrowserCaptureKeepAlive;
  }

  class OverlayWindow;
  class WgcCaptureJob;
  class Win32OverlayAdapter;
  class WindowsCaptureSource;
  class WindowsRuntimeFactory;

  /// Snapshot of concrete native work counters; no owning handles escape.
  struct WindowsRuntimeDiagnostics final {
    core::Extent allocation;
    std::uint64_t resource_generation{};
    core::BridgeMode bridge_mode{core::BridgeMode::CpuCopy};
    std::uint64_t captured_frames{};
    std::uint64_t presented_frames{};
    std::uint64_t dropped_frames{};
    std::size_t slots_in_flight{};
    std::size_t presentations_in_flight{};
    bool presentation_suspended{};
    std::optional<render::PresentationSnapshot> presentation;
  };

  /// @brief Own one replica's native resources and their ordered retirement.
  ///
  /// All port operations run on the owning main thread. Capture delegates reach
  /// separate callback state and request ticks; this aggregate consumes their
  /// facts and reports portable events without deciding use-case precedence.
  /// Source identity survives graphics rebuilds, while resource generations
  /// distinguish replacement allocations. Published ownership ends only after
  /// every independent callback, checkout, graphics, window, and helper barrier
  /// permits release; a timeout or contained exception is not that permission.
  class WindowsReplicaRuntime final : public app::IReplicaRuntime {
  public:
    using WakeMainLoop = std::function<void()>;

    /// Published instances must have emitted safe Stopped before destruction.
    ~WindowsReplicaRuntime() noexcept override;

    WindowsReplicaRuntime(const WindowsReplicaRuntime &) = delete;
    WindowsReplicaRuntime &operator=(const WindowsReplicaRuntime &) = delete;
    WindowsReplicaRuntime(WindowsReplicaRuntime &&) = delete;
    WindowsReplicaRuntime &operator=(WindowsReplicaRuntime &&) = delete;

    /// @brief Apply native settings and commit stored settings only after verification.
    ///
    /// Mode, border, client placement, and optional helper activation participate
    /// in the transaction. Failure with verified restoration reports a platform
    /// operation error. Unverifiable state or an exception reports RuntimeStateLost
    /// even though rollback is attempted; the caller must handle that uncertainty.
    [[nodiscard]] std::expected<void, core::AppError>
    apply(const core::ReplicaSettings &settings) noexcept override;
    /// @brief Advance capture, presentation, recovery, and retained-owner cleanup.
    ///
    /// Tick consumes coalesced native facts, schedules bounded work, and returns
    /// the earliest required observation time. Continue ticking after stop so
    /// callback and GPU retirement can be proved. Wakes are hints; a deadline
    /// keeps cleanup observable even when the final callback wake precedes its
    /// count reaching zero. Unexpected exceptions retain owners and schedule drain.
    [[nodiscard]] app::RuntimeTickResult
    tick(std::chrono::steady_clock::time_point now) noexcept override;
    /// @brief Initiate terminal cleanup without joining callback or GPU work.
    ///
    /// Repeated requests are harmless. Native stop calls can still fail or take
    /// time; ownership remains here until later ticks report Stopped. The caller
    /// must retain both the runtime and any host borrowed by its wake callable.
    void request_stop() noexcept override;
    /// Read counters while the render stack exists; unavailable after its release.
    [[nodiscard]] std::optional<WindowsRuntimeDiagnostics>
    diagnostics() const noexcept;

  private:
    struct ConstructionToken final {
    private:
      ConstructionToken() = default;
      friend class WindowsRuntimeFactory;
    };
    struct TickCandidates;
    struct SettingsOperations;

    enum class DeviceRecoveryPhase { Inactive,
                                     Draining,
                                     WaitingToRebuild };

  public:
    /// @brief Adopt a complete inactive aggregate through the factory's passkey.
    ///
    /// Public visibility permits make_unique construction; the private token
    /// restricts who may establish the dependency and publication invariants.
    /// Unique owners transfer here together before native capture is started.
    WindowsReplicaRuntime(
      ConstructionToken, std::unique_ptr<WindowsCaptureSource> source, std::unique_ptr<OverlayWindow> overlay, std::unique_ptr<Win32OverlayAdapter> adapter, std::unique_ptr<render::WindowsRenderStack> render_stack, std::unique_ptr<WgcCaptureJob> capture_job, std::unique_ptr<features::BrowserCaptureKeepAlive> browser_keep_alive, core::ReplicaSettings settings, core::Extent allocation, core::CaptureExclusionStatus capture_exclusion, render::WindowsBridgePreference bridge_preference, std::uint32_t pool_buffer_count, WakeMainLoop wake_main_loop) noexcept;

  private:
    friend class WindowsRuntimeFactory;
    [[nodiscard]] std::expected<void, core::AppError>
    activate_for_publication() noexcept;
    void cleanup_unpublished_activation_failure() noexcept;

    void add_runtime_events(app::RuntimeTickResult &result,
                            TickCandidates &candidates) noexcept;
    void begin_source_job_stop() noexcept;
    void
    begin_device_recovery(render::DeviceLossKind kind,
                          std::chrono::steady_clock::time_point now) noexcept;
    void progress_source_reconfiguration(TickCandidates &candidates,
                                         render::DrainProgress &progress);
    void progress_device_recovery(
      std::chrono::steady_clock::time_point now,
      const render::RenderTickResult &render_result,
      TickCandidates &candidates,
      render::DrainProgress &progress,
      std::optional<std::chrono::steady_clock::time_point> &deadline);
    void attempt_device_rebuild(
      std::chrono::steady_clock::time_point now, TickCandidates &candidates, render::DrainProgress &progress, std::optional<std::chrono::steady_clock::time_point> &deadline);
    void progress_stop(const render::RenderTickResult &render_result,
                       TickCandidates &candidates,
                       render::DrainProgress &progress) noexcept;
    [[nodiscard]] bool
    cleanup_pending(bool render_cleanup_pending) const noexcept;
    [[nodiscard]] bool cleanup_uncertain() const noexcept;
    void deactivate_browser_helper(TickCandidates &candidates) noexcept;
    [[nodiscard]] bool helper_activation_allowed() const noexcept;
    [[nodiscard]] bool
    helper_should_activate(const core::ReplicaSettings &settings) const noexcept;

    /// @brief Publish usable source geometry and advance one bounded selection gesture.
    /// The presented snapshot fixes the client-to-source mapping. Lifecycle or
    /// geometry changes end that gesture and restore the accepted preferences.
    void progress_region_selection(const render::RenderTickResult &render_result,
                                   TickCandidates &candidates) noexcept;
    void invalidate_region_content(TickCandidates &candidates) noexcept;
    void cancel_region_selection(TickCandidates &candidates) noexcept;
    void update_presentation_extent() noexcept;

    // Declaration order is the stable dependency order. Normal teardown resets
    // these owners in reverse: job, render, adapter, overlay, then source.
    std::unique_ptr<WindowsCaptureSource> source_;
    std::unique_ptr<OverlayWindow> overlay_;
    std::unique_ptr<Win32OverlayAdapter> adapter_;
    std::unique_ptr<render::WindowsRenderStack> render_stack_;
    std::unique_ptr<WgcCaptureJob> capture_job_;
    // Independent of the overlay/source/GPU chain and retained through its own drain.
    std::unique_ptr<features::BrowserCaptureKeepAlive> browser_keep_alive_;

    core::FrameScheduler scheduler_;
    core::ReplicaSettings settings_;
    core::Extent allocation_;
    // This counter belongs to the stable runtime, not a rebuilt device/stack.
    std::uint64_t resource_generation_{1};
    const render::WindowsBridgePreference bridge_preference_;
    const std::uint32_t pool_buffer_count_;
    WakeMainLoop wake_main_loop_;

    std::optional<core::CaptureExclusionStatus> initial_capture_exclusion_;
    detail::RuntimeBridgeEventState bridge_event_state_;
    std::optional<core::AppError> pending_failed_event_;
    bool pending_overlay_close_event_{};
    bool pending_source_lost_event_{};
    detail::RuntimeRecoveryEventState recovery_event_state_;
    detail::SourceReconfigurationPhase reconfiguration_phase_{
      detail::SourceReconfigurationPhase::Inactive};
    std::optional<core::Extent> pending_allocation_;
    DeviceRecoveryPhase device_recovery_phase_{DeviceRecoveryPhase::Inactive};
    render::GraphicsRecoveryEpisodeState device_recovery_episode_;
    std::optional<std::chrono::steady_clock::time_point>
      device_recovery_started_at_;
    bool replacement_awaiting_present_{};
    // These independent facts select different native retirement permissions.
    bool vulkan_device_lost_{};
    bool d3_device_removed_{};
    bool source_disconnected_{};
    bool source_lost_published_{};
    bool overlay_close_published_{};
    bool source_job_stopping_{};
    bool stop_requested_{};
    bool stopped_{};
    bool published_{};
    bool helper_activation_ready_{};
    render::RenderTickResult last_render_result_;
    std::optional<core::Extent> presented_content_;
    std::optional<core::Extent> awaited_content_;
    std::optional<render::PresentationSnapshot> selection_mapping_;
    std::optional<core::Extent> selection_client_extent_;
    std::optional<core::PixelRegion> selection_preview_;
    bool selection_result_pending_{};
    // One timer covers capture, adapter, source wake, and graphics uncertainty.
    detail::RuntimeDrainState drain_state_;
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_REPLICA_RUNTIME_HPP
