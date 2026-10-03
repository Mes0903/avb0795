/// @file
/// @brief Linux replica ownership and native-free retirement observations.
#ifndef AVB0795_LINUX_REPLICA_RUNTIME_HPP
#define AVB0795_LINUX_REPLICA_RUNTIME_HPP
#include "avb/app/ports.hpp"
#include "render/graphics_retry_policy.hpp"
#include "render/replica_presentation.hpp"
#include "avb/core/frame_scheduler.hpp"
#include "platform/linux/pipewire_capture.hpp"
#include "platform/linux/x11_overlay_adapter.hpp"
#include "render/vulkan_renderer.hpp"
#include <functional>
namespace avb::linux_platform::detail {
  /// @brief Main-thread source bounds certified only by retired current presentation.
  /// The renderer filters stale presentation sequences. This scalar owner neither
  /// polls fences nor accepts producer dimensions as displayed geometry.
  struct RetiredContent final {
    core::Extent extent{};
    bool pending{};
    /// @brief Retain a bounded content edge across higher-priority lifecycle arbitration.
    /// A deferred fact remains owned here until a tick can expose it to the application.
    [[nodiscard]] std::optional<core::Extent> take_change(bool lifecycle_pending) noexcept;
    [[nodiscard]] bool observe(render::PresentationSnapshot) noexcept;
    [[nodiscard]] bool invalidate() noexcept;
  };
  /// @brief Owner-local watchdog; polls and construction retries cannot extend it.
  /// Sticky uncertainty ends only at an explicit empty-owner proof. This contains
  /// no native owners and cannot itself authorize a callback, worker or GPU release.
  struct RuntimeOwnershipClock final {
    using Time = std::chrono::steady_clock::time_point;
    std::optional<Time> last_progress;
    bool uncertain{};
    [[nodiscard]] render::RuntimeDrainDecision observe(Time, bool, bool, render::DrainProgress) noexcept;
  };
}    // namespace avb::linux_platform::detail
namespace avb::linux_platform {
  class LinuxRuntimeFactory;
  /// @brief Read-only scalar evidence from this concrete runtime's normal tick.
  /// Main-thread copies only: no native owner, pixels, source name or second frame
  /// consumer is exposed. capture_instance is nonzero and unique within the owning
  /// factory; Stop/Start creates a new instance. Publication counts accepted pool
  /// frames, while delivered_stamp is updated only after successful bridge copy.
  /// Retired presents include retained redraw and cannot stand in for source intake.
  /// The aggregate adds each renderer cumulative increment once; actual renderer
  /// replacement resets only that renderer baseline and preserves the lifetime total.
  /// Producer revision is observed with publication facts, even before the graphics
  /// ring admits it; graphics epoch is a separate discontinuity. Values
  /// survive acknowledged Stop, but only stopped certifies this aggregate's release;
  /// absence of a later snapshot cannot establish cleanup or zero pending work.
  struct LinuxSourceObservation final {
    std::uint64_t capture_instance{}, producer_revision{}, graphics_epoch{};
    std::uint64_t published_source_sequence{}, successful_present_retirements{};
    render::FrameStamp delivered_stamp{};
    bool stopped{};
  };
  /// @brief Main-thread GLFW owner, destroyed only after adapter/surface retirement.
  struct ReplicaWindowDeleter {
    void operator()(GLFWwindow *) const noexcept;
  };
  using ReplicaWindow = std::unique_ptr<GLFWwindow, ReplicaWindowDeleter>;
  /// @brief Concrete consumer aggregate retained through independent native proofs.
  /// Capture callbacks only wake the host. Tick orders present observation, CPU
  /// delivery, submission retirement and WSI/draw. Device repair uses a private
  /// graphics epoch, never producer revision. Stop initiates asynchronous retirement;
  /// only Stopped permits destruction of a published instance. Stalled retains it.
  class LinuxReplicaRuntime final : public app::IReplicaRuntime {
    struct ConstructionToken {
    private:
      ConstructionToken() = default;
      friend class LinuxRuntimeFactory;
    };

  public:
    /// @brief Prepare all runtime storage before capture transfer using factory passkey.
    /// Dependencies arrive unpublished and without submissions; destruction is then
    /// safe even if registry commit fails. Only attach_capture may publish this owner.
    LinuxReplicaRuntime(ConstructionToken, ReplicaWindow, std::unique_ptr<X11OverlayAdapter>, std::unique_ptr<render::VulkanRenderer>, core::ReplicaSettings, std::uint64_t revision, std::uint64_t ring_serial, std::function<void()> wake, diagnostics::PerformanceReport *, std::uint64_t observation_instance) noexcept;
    ~LinuxReplicaRuntime() override;
    /// @brief Accept native requests before the no-throw renderer/settings commit.
    /// Ordinary rejection preserves verified settings; unknown rollback starts drain.
    [[nodiscard]] std::expected<void, core::AppError> apply(const core::ReplicaSettings &) noexcept override;
    /// @brief Advance independent producer/presentation/submission proof without waiting.
    /// Continue ticks after Stop/loss. Unexpected failure retains owners and requests
    /// drain; deadlines are observation requests and never completion evidence.
    [[nodiscard]] app::RuntimeTickResult tick(std::chrono::steady_clock::time_point) noexcept override;
    /// @brief Idempotently close admission and initiate cleanup without a join.
    /// Capture callback/thread/wake completion and independent GPU proof remain owed.
    void request_stop() noexcept override;
    /// @brief Copy bounded diagnostic facts without consuming or controlling capture.
    /// Read on the owning main thread between ticks. This snapshot neither proves
    /// changing source pixels nor authorizes retirement of any outstanding owner.
    [[nodiscard]] LinuxSourceObservation source_observation() const noexcept;

  private:
    friend class LinuxRuntimeFactory;
    /// @brief No-throw final attachment after registry has bound the exact first frame.
    /// No settings copy, acknowledgment or allocation may follow this transfer.
    void attach_capture(std::unique_ptr<PipeWireCapture>) noexcept;
    /// @brief Withdraw displayed geometry and restore transient selection preferences.
    /// Zero snapshots reset adapter sequence history across renderer replacements.
    void invalidate_mapping() noexcept;
    /// @brief Commit adapter-certified selection completion to runtime/render values.
    /// Native restoration has already succeeded; this value-only step must precede
    /// content/result publication so accepted transient state cannot remain armed.
    void complete_selection() noexcept;
    /// @brief Retain a terminal state-loss fact while initiating safe producer/graphics drain.
    /// Does not report Stopped or destroy any asynchronous owner.
    void fail(core::AppError) noexcept;
    /// @brief Explicitly loss-drain old graphics while preserving healthy CPU capture.
    /// Stop, source loss and terminal failure prevent any replacement attempt.
    void begin_device_recovery(std::chrono::steady_clock::time_point) noexcept;
    /// @brief Replace only an exact storage-ready producer allocation after image detach.
    /// Superseded acknowledgment leaves admission closed; partial graphics failure
    /// enters retained drain rather than releasing descriptor-referenced images.
    void progress_reconfiguration(std::chrono::steady_clock::time_point, const detail::CpuPoolFacts &, render::DrainProgress &);
    /// @brief Spend the existing three-attempt budget on local unpublished graphics.
    /// The private graphics epoch advances only on installation; ring generation
    /// remains producer revision. Construction is not ownership retirement progress.
    void progress_device_recovery(std::chrono::steady_clock::time_point, const detail::CpuPoolFacts &, render::VulkanTickResult &, render::DrainProgress &, std::optional<std::chrono::steady_clock::time_point> &);
    /// Stable dependency order; reverse release requires the explicit proof in tick.
    ReplicaWindow window_;
    std::unique_ptr<X11OverlayAdapter> adapter_;
    std::unique_ptr<render::VulkanRenderer> renderer_;
    std::unique_ptr<PipeWireCapture> capture_;
    std::optional<LinuxCapturedFrame> retry_frame_;
    core::ReplicaSettings settings_;
    core::FrameScheduler scheduler_;
    std::function<void()> wake_;
    diagnostics::PerformanceReport *performance_{};
    std::uint64_t observation_instance_{}, producer_revision_{}, ring_serial_{}, graphics_epoch_{1};
    std::uint64_t published_source_sequence_{}, successful_present_retirements_{}, renderer_presented_baseline_{};
    std::uint64_t observed_producer_revision_{};
    render::FrameStamp delivered_stamp_{};
    detail::RetiredContent content_;
    detail::RuntimeOwnershipClock ownership_clock_;
    render::GraphicsRecoveryEpisodeState recovery_episode_;
    std::optional<std::chrono::steady_clock::time_point> recovery_started_;
    std::size_t prior_slots_{}, prior_presents_{};
    bool published_{}, stop_requested_{}, stopped_{}, source_lost_{}, source_lost_reported_{}, close_reported_{};
    bool terminal_failure_{}, reconfiguring_{}, device_recovering_{}, loss_drain_applied_{}, initial_status_{true};
    std::optional<core::AppError> failed_;
    bool recovery_started_event_{}, recovered_event_{};
    core::RuntimeDrainStatus drain_status_{core::RuntimeDrainStatus::Normal};
  };
  namespace detail {
    /// @brief Classify a known renderer ring/detach result, never a producer error.
    /// Caller provenance is required; matching arbitrary native integers gives no
    /// Vulkan loss authority. Concrete callers latch loss before draining graphics.
    [[nodiscard]] bool is_graphics_device_loss(core::AppError) noexcept;
    /// @brief Build a complete unpublished CPU ring, retaining partial graphics on error.
    /// Known ring-operation device loss is latched before propagating the typed error.
    /// The caller destroys an unpublished renderer or loss-drains/rebuilds a live one;
    /// no capture/native producer result is classified at this graphics boundary.
    [[nodiscard]] std::uint64_t prepare_cpu_ring(render::VulkanRenderer &, core::Extent, std::uint64_t);
    /// @brief Query drawable size on the initialized GLFW owning thread.
    [[nodiscard]] core::Extent replica_framebuffer(GLFWwindow *) noexcept;
  }    // namespace detail
}    // namespace avb::linux_platform
#endif
