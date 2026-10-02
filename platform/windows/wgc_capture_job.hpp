/// @file
/// @brief One WGC session/pool generation, contained intake, and staged stop.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_WGC_CAPTURE_JOB_HPP
#define AVB0795_PLATFORM_WINDOWS_WGC_CAPTURE_JOB_HPP

#include "platform/windows/windows_capture_source.hpp"
#include "platform/windows/windows_captured_frame.hpp"

#include <d3d11.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

namespace avb::platform::windows {

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

  /// @brief Own a capture session, frame pool, and newest-only checkout mailbox.
  ///
  /// Main-thread lifecycle operations coordinate with free-threaded WinRT
  /// delegates through shared callback state. A registration epoch rejects late
  /// handlers; resource generations identify which allocation a delivered frame
  /// belongs to. Neither token replaces the caller's obligation to retire GPU
  /// uses and externally held checkouts before recreating or destroying the pool.
  class WgcCaptureJob final {
  public:
    using WakeMainLoop = std::function<void()>;

    /// @brief Prepare native capture and callback state with intake still closed.
    ///
    /// The source and graphics owner outlive this job. The runtime supplies the
    /// resource generation and a pool count covering destination slots plus
    /// intake and mailbox checkout headroom. The copied wake callable only asks
    /// for a main-thread tick; borrowed targets must survive callback drain.
    /// A closed source, invalid count, empty wake, or native construction failure
    /// returns a typed error without publishing an active runtime.
    [[nodiscard]] static std::expected<std::unique_ptr<WgcCaptureJob>,
                                       core::AppError>
    create_inactive(WindowsCaptureSource &source, ID3D11Device &d3d_device, std::uint64_t resource_generation, std::uint32_t pool_buffer_count, WakeMainLoop wake_main_loop);

    /// @brief Return the checked extent used for the initial pool allocation.
    ///
    /// Factory construction and device recovery use this value to initialize
    /// matching graphics state. It is not a live size query; subsequent changes
    /// require the reconfiguration barrier and remain separate from this value.
    [[nodiscard]] core::Extent initial_allocation() const noexcept;

    /// @brief Release a safely drained job, or an unpublished construction partial.
    ///
    /// Once publication starts, destruction terminates the process if internal
    /// stop safety has not been proved. Internal quiescence covers callbacks,
    /// mailbox, and native cleanup; it cannot count checkouts already transferred
    /// to rendering work or external consumers.
    /// @pre A published job has stopped, stop_quiescent() is true, and all
    /// externally held frames and their GPU uses have retired.
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
    /// @brief Attempt capture start once and open intake only after it succeeds.
    ///
    /// The sticky source check precedes the native start call. Success commits
    /// the job to published lifetime rules; it does not guarantee that a frame
    /// arrives or that the source cannot close immediately afterward.
    [[nodiscard]] std::expected<void, core::AppError> start_for_publication();
    /// Close the old epoch and revoke its handler; later ticks prove quiescence.
    [[nodiscard]] std::expected<void, core::AppError> begin_reconfigure();
    /// Include final borrowed wake execution in the callback-lifetime proof.
    [[nodiscard]] bool frame_callbacks_quiescent() const noexcept;
    /// @brief Recreate the pool and register the prepared epoch without opening it.
    ///
    /// The method checks internal callback and mailbox quiescence. A native
    /// failure may occur after recreation has begun, so failure is not a promise
    /// that the old allocation remains usable.
    /// @pre The owner has retired all external checkouts and GPU uses, and has
    /// successfully begun reconfiguration with a matching new resource generation.
    [[nodiscard]] std::expected<void, core::AppError>
    prepare_reconfigure(core::Extent allocation, std::uint64_t generation);
    /// Open the prepared epoch after the runtime's final sticky source check.
    void commit_reconfigure() noexcept;
    /// @brief Close intake and attempt handler revocation and session close once.
    ///
    /// The method does not join callbacks. Native calls may still take time or
    /// fail; retained cleanup uncertainty keeps stop_quiescent() false and must
    /// keep the job owned instead of treating exception containment as success.
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

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WGC_CAPTURE_JOB_HPP
