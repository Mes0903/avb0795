/// @file
/// @brief One WGC session/pool generation, contained intake, and nonblocking stop.
/// Native implementation details remain below the portable application ports.

#ifndef AIRELINNA_PLATFORM_WINDOWS_WGC_CAPTURE_JOB_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WGC_CAPTURE_JOB_HPP

#include "platform/windows/windows_capture_source.hpp"
#include "platform/windows/windows_captured_frame.hpp"

#include <d3d11.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

namespace airelinna::platform::windows {

  /// Fixed callback failure vocabulary; no dynamically allocated diagnostic text.
  enum class CaptureFatalCategory {
    None,
    ProjectionFailure,
    OutOfMemory,
    UnknownCallbackFailure,
    FormatMismatch,
    InvalidFrameMetadata,
  };

  /// Sticky capture failure plus the original numeric native detail.
  struct CaptureFatalSignal final {
    CaptureFatalCategory category{CaptureFatalCategory::None};
    std::int64_t native_detail{};

    friend bool operator==(const CaptureFatalSignal &,
                           const CaptureFatalSignal &) = default;
  };

  /// One tick's coalesced edges plus persistent fatal state; never owns pixels.
  struct CaptureSignalBatch final {
    bool fatal_active{};
    bool fatal_edge{};
    CaptureFatalSignal fatal{};
    std::optional<core::Extent> resize;
  };

  /// Native stop steps whose failure leaves callback/session ownership uncertain.
  enum class CaptureCleanupCategory {
    FrameArrivedRevocationFailure,
    CaptureSessionCloseFailure,
  };

  /// Retained stop diagnostic that forbids claiming internal cleanup is complete.
  struct CaptureCleanupFailure final {
    CaptureCleanupCategory category{
      CaptureCleanupCategory::FrameArrivedRevocationFailure};
    std::int64_t native_detail{};

    friend bool operator==(const CaptureCleanupFailure &,
                           const CaptureCleanupFailure &) = default;
  };

  /// Owns one session/pool and newest-only raw-frame mailbox.
  /// Main-thread consumers coordinate drain; WinRT delegates hold weak typed state.
  class WgcCaptureJob final {
  public:
    using WakeMainLoop = std::function<void()>;

    // Concrete P2/P3 construction boundary. The P3 runtime owns source and its
    // monotonically increasing resource generation; its render stack owns
    // d3d_device. Both borrowed objects must outlive this job. P3 supplies a
    // non-zero, representable pool count sized to its bounded destination slots
    // plus callback/local and mailbox checkout headroom. wake_main_loop only
    // requests a main-thread tick and is not an application event sink.
    /// Build all native callback state with frame intake still closed.
    [[nodiscard]] static std::expected<std::unique_ptr<WgcCaptureJob>,
                                       core::AppError>
    create_inactive(WindowsCaptureSource &source, ID3D11Device &d3d_device, std::uint64_t resource_generation, std::uint32_t pool_buffer_count, WakeMainLoop wake_main_loop);

    // The exact checked extent used to create this job's initial pool. Factory
    // and device recovery use it to initialize matching render/runtime state;
    // later size changes still go through the runtime reconfiguration barrier.
    /// Return the allocation validated during this job's construction.
    [[nodiscard]] core::Extent initial_allocation() const noexcept;

    // Runtime owners must request stop and retain the job until stop_quiescent(),
    // and must release every WindowsCapturedFrame handed to external/renderer
    // ownership. stop_quiescent() proves only internal callback, mailbox, and
    // native cleanup; once publication starts, destruction fail-fasts if that
    // internal proof is absent. An inactive factory partial may unwind normally.
    ~WgcCaptureJob();

    WgcCaptureJob(const WgcCaptureJob &) = delete;
    WgcCaptureJob &operator=(const WgcCaptureJob &) = delete;
    WgcCaptureJob(WgcCaptureJob &&) = delete;
    WgcCaptureJob &operator=(WgcCaptureJob &&) = delete;

    // P3 calls these synchronously from its main-thread tick. Frames and fixed
    // typed signals cross this boundary; callback state, native event tokens,
    // and application events do not.
    /// Observe mailbox readiness without transferring its checkout.
    [[nodiscard]] bool has_latest() const noexcept;
    /// Move the newest checkout to the main-thread consumer; older frames are dropped.
    [[nodiscard]] std::optional<WindowsCapturedFrame> take_latest();
    /// Consume resize/fatal edges while preserving the sticky fatal fact.
    [[nodiscard]] CaptureSignalBatch drain_signals();
    /// Claim one pending arrival and perform a pool-count-bounded intake pass.
    void service_pending_arrival() noexcept;
    /// Check sticky source closure, StartCapture, then open the pending handshake.
    [[nodiscard]] std::expected<void, core::AppError> start_for_publication();
    /// Close the old epoch and revoke its handler; later ticks prove quiescence.
    [[nodiscard]] std::expected<void, core::AppError> begin_reconfigure();
    /// Include final borrowed wake execution in the callback-lifetime proof.
    [[nodiscard]] bool frame_callbacks_quiescent() const noexcept;
    /// Recreate only after old mailbox/checkouts/render allocation are safely retired.
    [[nodiscard]] std::expected<void, core::AppError>
    prepare_reconfigure(core::Extent allocation, std::uint64_t generation);
    /// Open the prepared epoch after the runtime's final sticky source check.
    void commit_reconfigure() noexcept;
    /// Initiate terminal cleanup, containing revoke/Close failure without waiting.
    void request_stop() noexcept;
    /// Prove internal callback/mailbox/native safety, not external render-held checkouts.
    [[nodiscard]] bool stop_quiescent() const noexcept;
    /// Expose retained cleanup uncertainty to the runtime's existing drain policy.
    [[nodiscard]] std::optional<CaptureCleanupFailure>
    cleanup_failure() const noexcept;

  private:
    class Impl;
    explicit WgcCaptureJob(std::unique_ptr<Impl> implementation, core::Extent initial_allocation) noexcept;

    std::unique_ptr<Impl> implementation_;
    const core::Extent initial_allocation_;
  };

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WGC_CAPTURE_JOB_HPP
