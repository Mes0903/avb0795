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

  enum class CaptureFatalCategory {
    None,
    ProjectionFailure,
    OutOfMemory,
    UnknownCallbackFailure,
    FormatMismatch,
    InvalidFrameMetadata,
  };

  struct CaptureFatalSignal final {
    CaptureFatalCategory category{CaptureFatalCategory::None};
    std::int64_t native_detail{};

    friend bool operator==(const CaptureFatalSignal &,
                           const CaptureFatalSignal &) = default;
  };

  struct CaptureSignalBatch final {
    bool fatal_active{};
    bool fatal_edge{};
    CaptureFatalSignal fatal{};
    std::optional<core::Extent> resize;
  };

  enum class CaptureCleanupCategory {
    FrameArrivedRevocationFailure,
    CaptureSessionCloseFailure,
  };

  struct CaptureCleanupFailure final {
    CaptureCleanupCategory category{
      CaptureCleanupCategory::FrameArrivedRevocationFailure};
    std::int64_t native_detail{};

    friend bool operator==(const CaptureCleanupFailure &,
                           const CaptureCleanupFailure &) = default;
  };

  class WgcCaptureJob final {
  public:
    using WakeMainLoop = std::function<void()>;

    // Concrete P2/P3 construction boundary. The P3 runtime owns source and its
    // monotonically increasing resource generation; its render stack owns
    // d3d_device. Both borrowed objects must outlive this job. P3 supplies a
    // non-zero, representable pool count sized to its bounded destination slots
    // plus callback/local and mailbox checkout headroom. wake_main_loop only
    // requests a main-thread tick and is not an application event sink.
    [[nodiscard]] static std::expected<std::unique_ptr<WgcCaptureJob>,
                                       core::AppError>
    create_inactive(WindowsCaptureSource &source, ID3D11Device &d3d_device, std::uint64_t resource_generation, std::uint32_t pool_buffer_count, WakeMainLoop wake_main_loop);

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
    [[nodiscard]] bool has_latest() const noexcept;
    [[nodiscard]] std::optional<WindowsCapturedFrame> take_latest();
    [[nodiscard]] CaptureSignalBatch drain_signals();
    void service_pending_arrival() noexcept;
    [[nodiscard]] std::expected<void, core::AppError> start_for_publication();
    [[nodiscard]] std::expected<void, core::AppError> begin_reconfigure();
    [[nodiscard]] bool frame_callbacks_quiescent() const noexcept;
    [[nodiscard]] std::expected<void, core::AppError>
    prepare_reconfigure(core::Extent allocation, std::uint64_t generation);
    void commit_reconfigure() noexcept;
    void request_stop() noexcept;
    [[nodiscard]] bool stop_quiescent() const noexcept;
    [[nodiscard]] std::optional<CaptureCleanupFailure>
    cleanup_failure() const noexcept;

  private:
    class Impl;
    explicit WgcCaptureJob(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
  };

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WGC_CAPTURE_JOB_HPP
