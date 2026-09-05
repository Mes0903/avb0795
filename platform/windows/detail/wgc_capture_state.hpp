#ifndef AIRELINNA_PLATFORM_WINDOWS_DETAIL_WGC_CAPTURE_STATE_HPP
#define AIRELINNA_PLATFORM_WINDOWS_DETAIL_WGC_CAPTURE_STATE_HPP

#include "platform/windows/detail/srw_lock.hpp"
#include "platform/windows/wgc_capture_job.hpp"

#include <winerror.h>
#include <winrt/base.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <new>
#include <optional>
#include <utility>

namespace airelinna::platform::windows::detail {

  using CapturePixelFormat =
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

  struct FrameObservation final {
    std::uint64_t resource_generation{};
    core::Extent allocation_extent{};
    core::Extent content_size{};
    CapturePixelFormat surface_format{};
  };

  struct FrameObservationResult final {
    bool accepted{};
    bool signal_edge{};
  };

  [[nodiscard]] core::Extent
  copyable_content_extent(core::Extent content, core::Extent allocation) noexcept;

  class FrameCallbackPolicy final {
  public:
    class Admission final {
    public:
      ~Admission();

      Admission(const Admission &) = delete;
      Admission &operator=(const Admission &) = delete;
      Admission(Admission &&other) noexcept;
      Admission &operator=(Admission &&) = delete;

      [[nodiscard]] bool is_open() const noexcept;
      void record_pending() noexcept;
      void wake_if_open() noexcept;

    private:
      friend class FrameCallbackPolicy;
      Admission(FrameCallbackPolicy &owner, std::uint64_t epoch) noexcept;

      FrameCallbackPolicy *owner_{};
      std::uint64_t epoch_{};
    };

    FrameCallbackPolicy(std::uint32_t pool_buffer_count, core::Extent allocation, std::uint64_t resource_generation, CapturePixelFormat pool_format, std::function<void()> wake_main_loop);

    FrameCallbackPolicy(const FrameCallbackPolicy &) = delete;
    FrameCallbackPolicy &operator=(const FrameCallbackPolicy &) = delete;

    [[nodiscard]] std::uint64_t current_epoch() const noexcept;
    [[nodiscard]] bool arrival_pending() const noexcept;
    [[nodiscard]] bool admission_open() const noexcept;
    [[nodiscard]] std::uint32_t pool_buffer_count() const noexcept;
    [[nodiscard]] std::optional<Admission>
    enter(std::uint64_t registered_epoch) noexcept;

    void open_current_epoch() noexcept;
    [[nodiscard]] bool claim_pending_arrival() noexcept;
    void notify_mailbox_ready() noexcept;

    [[nodiscard]] FrameObservationResult
    observe_frame(const FrameObservation &frame) noexcept;
    void record_projection_failure(std::int64_t native_detail) noexcept;
    void record_out_of_memory() noexcept;
    void record_unknown_callback_failure() noexcept;
    [[nodiscard]] CaptureSignalBatch drain_signals() noexcept;

    [[nodiscard]] std::uint64_t close_for_reconfigure() noexcept;
    [[nodiscard]] bool frame_callbacks_quiescent() const noexcept;
    [[nodiscard]] std::expected<void, core::AppError>
    prepare_current_epoch(core::Extent allocation,
                          std::uint64_t resource_generation) noexcept;
    void request_terminal_stop() noexcept;
    [[nodiscard]] bool terminal() const noexcept;

  private:
    friend class Admission;

    [[nodiscard]] bool epoch_matches(std::uint64_t epoch) const noexcept;
    void leave_callback() noexcept;
    void record_pending(std::uint64_t epoch) noexcept;
    void wake_if_open(std::uint64_t epoch) noexcept;
    void latch_fatal(CaptureFatalCategory category,
                     std::int64_t native_detail) noexcept;
    void wake_noexcept() noexcept;

    const std::uint32_t pool_buffer_count_;
    core::Extent allocation_;
    std::uint64_t resource_generation_;
    const CapturePixelFormat pool_format_;
    std::function<void()> wake_main_loop_;
    std::atomic_uint64_t epoch_{1};
    std::atomic_bool admission_open_{};
    std::atomic_bool terminal_{};
    std::atomic_bool arrival_pending_{};
    mutable ExclusiveSrwLock callback_admission_lock_;
    std::size_t callbacks_in_flight_{};
    bool quiescence_wake_requested_{};
    bool final_quiescence_wake_claimed_{};
    mutable ExclusiveSrwLock signal_lock_;
    std::optional<CaptureFatalSignal> fatal_;
    bool fatal_edge_{};
    bool resize_edge_{};
    std::optional<core::Extent> resize_;
  };

  class PublicationGate final {
  public:
    template <class SourceClosed, class StartCapture, class OpenAdmission>
    [[nodiscard]] std::expected<void, core::AppError>
    start_for_publication(SourceClosed &&source_closed,
                          StartCapture &&start_capture,
                          OpenAdmission &&open_admission)
    {
      if (attempted_) {
        return std::unexpected{
          core::AppError{.code = core::AppErrorCode::InvalidCommand}};
      }
      attempted_ = true;
      if (std::invoke(std::forward<SourceClosed>(source_closed))) {
        return std::unexpected{
          core::AppError{.code = core::AppErrorCode::SourceUnavailable}};
      }
      try {
        std::invoke(std::forward<StartCapture>(start_capture));
      } catch (const winrt::hresult_error &error) {
        const auto code = error.code().value;
        return std::unexpected{core::AppError{
          .code = code == E_ACCESSDENIED
                    ? core::AppErrorCode::PermissionDenied
                    : core::AppErrorCode::RuntimeConstructionFailure,
          .native_detail = code}};
      } catch (const std::bad_alloc &) {
        return std::unexpected{
          core::AppError{.code = core::AppErrorCode::RuntimeConstructionFailure,
                         .native_detail = E_OUTOFMEMORY}};
      } catch (...) {
        return std::unexpected{
          core::AppError{.code = core::AppErrorCode::RuntimeConstructionFailure,
                         .native_detail = E_FAIL}};
      }
      started_ = true;
      std::invoke(std::forward<OpenAdmission>(open_admission));
      return {};
    }

    [[nodiscard]] bool started() const noexcept { return started_; }

  private:
    bool attempted_{};
    bool started_{};
  };

  class CaptureJobStopState final {
  public:
    template <class RevokeFrameArrived, class CloseSession>
    void request_stop(RevokeFrameArrived &&revoke_frame_arrived,
                      CloseSession &&close_session) noexcept
    {
      if (requested_) {
        return;
      }
      requested_ = true;
      try {
        std::invoke(std::forward<RevokeFrameArrived>(revoke_frame_arrived));
      } catch (...) {
        record_failure(CaptureCleanupCategory::FrameArrivedRevocationFailure,
                       current_exception_detail());
      }
      try {
        std::invoke(std::forward<CloseSession>(close_session));
      } catch (...) {
        record_failure(CaptureCleanupCategory::CaptureSessionCloseFailure,
                       current_exception_detail());
      }
      native_stop_succeeded_ = !cleanup_failure_.has_value();
    }

    [[nodiscard]] bool native_stop_succeeded() const noexcept;
    [[nodiscard]] std::optional<CaptureCleanupFailure>
    cleanup_failure() const noexcept;
    [[nodiscard]] bool stop_quiescent(bool callbacks_quiescent,
                                      bool mailbox_closed) const noexcept;

  private:
    [[nodiscard]] static std::int64_t current_exception_detail() noexcept;
    void record_failure(CaptureCleanupCategory category,
                        std::int64_t native_detail) noexcept;

    bool requested_{};
    bool native_stop_succeeded_{};
    std::optional<CaptureCleanupFailure> cleanup_failure_;
  };

  [[nodiscard]] core::AppError
  classify_wgc_construction_failure(bool source_closed,
                                    std::int64_t native_detail,
                                    bool permission_denied = false) noexcept;

  void enforce_wgc_job_destruction_invariant(bool safe_to_destroy) noexcept;

}    // namespace airelinna::platform::windows::detail

#endif    // AIRELINNA_PLATFORM_WINDOWS_DETAIL_WGC_CAPTURE_STATE_HPP
