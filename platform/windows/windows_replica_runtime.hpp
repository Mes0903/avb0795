/// @file
/// @brief Main-thread replica aggregate, native progress, and ordered safe teardown.
/// Native implementation details remain below the portable application ports.

#ifndef AIRELINNA_PLATFORM_WINDOWS_WINDOWS_REPLICA_RUNTIME_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WINDOWS_REPLICA_RUNTIME_HPP

#include "airelinna/app/ports.hpp"
#include "airelinna/core/frame_scheduler.hpp"
#include "platform/windows/detail/windows_runtime_policy.hpp"
#include "render/graphics_retry_policy.hpp"
#include "render/windows_render_stack.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace airelinna::platform::windows {

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
  };

  /// Main-thread owner of one replica and its native cleanup order.
  /// Converts capture/render facts to portable events without choosing lifecycle precedence.
  class WindowsReplicaRuntime final : public app::IReplicaRuntime {
  public:
    using WakeMainLoop = std::function<void()>;

    /// Published instances must have emitted safe Stopped before destruction.
    ~WindowsReplicaRuntime() noexcept override;

    WindowsReplicaRuntime(const WindowsReplicaRuntime &) = delete;
    WindowsReplicaRuntime &operator=(const WindowsReplicaRuntime &) = delete;
    WindowsReplicaRuntime(WindowsReplicaRuntime &&) = delete;
    WindowsReplicaRuntime &operator=(WindowsReplicaRuntime &&) = delete;

    /// Apply native mode/border/geometry transactionally; scheduler commits last.
    [[nodiscard]] std::expected<void, core::AppError>
    apply(const core::ReplicaSettings &settings) noexcept override;
    /// Advance bounded capture, rendering, recovery, and drain on the main thread.
    /// Return value-only facts and the earliest required future observation.
    [[nodiscard]] app::RuntimeTickResult
    tick(std::chrono::steady_clock::time_point now) noexcept override;
    /// Initiate retained-owner teardown; never synchronously wait for a callback/GPU.
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
    // Passkey-gated so std::make_unique can construct the aggregate while only
    // WindowsRuntimeFactory can create a ConstructionToken.
    /// Adopt the complete inactive aggregate; only the factory can issue the passkey.
    WindowsReplicaRuntime(
      ConstructionToken, std::unique_ptr<WindowsCaptureSource> source, std::unique_ptr<OverlayWindow> overlay, std::unique_ptr<Win32OverlayAdapter> adapter, std::unique_ptr<render::WindowsRenderStack> render_stack, std::unique_ptr<WgcCaptureJob> capture_job, core::ReplicaSettings settings, core::Extent allocation, core::CaptureExclusionStatus capture_exclusion, render::WindowsBridgePreference bridge_preference, std::uint32_t pool_buffer_count, WakeMainLoop wake_main_loop) noexcept;

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

    // Declaration order is the stable dependency order. Normal teardown resets
    // these owners in reverse: job, render, adapter, overlay, then source.
    std::unique_ptr<WindowsCaptureSource> source_;
    std::unique_ptr<OverlayWindow> overlay_;
    std::unique_ptr<Win32OverlayAdapter> adapter_;
    std::unique_ptr<render::WindowsRenderStack> render_stack_;
    std::unique_ptr<WgcCaptureJob> capture_job_;

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
    render::RenderTickResult last_render_result_;
    // One timer covers capture, adapter, source wake, and graphics uncertainty.
    detail::RuntimeDrainState drain_state_;
  };

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_REPLICA_RUNTIME_HPP
