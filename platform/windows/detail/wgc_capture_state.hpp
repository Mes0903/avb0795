/// @file
/// @brief Capture admission epochs, bounded latches, publication, and stop proofs.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_WGC_CAPTURE_STATE_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_WGC_CAPTURE_STATE_HPP

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

namespace avb::platform::windows::detail {

  using CapturePixelFormat =
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

  /// Projected native metadata; no frame ownership crosses into this policy value.
  struct FrameObservation final {
    std::uint64_t resource_generation{};
    core::Extent allocation_extent{};
    core::Extent content_size{};
    CapturePixelFormat surface_format{};
  };

  /// Intake acceptance and whether this observation created a new signal edge.
  struct FrameObservationResult final {
    bool accepted{};
    bool signal_edge{};
  };

  /// Clip the valid copy rectangle while retaining original ContentSize for resize.
  [[nodiscard]] core::Extent
  copyable_content_extent(core::Extent content, core::Extent allocation) noexcept;

  /// @brief Coordinate callback lifetime, intake readiness, and bounded signals.
  ///
  /// Matching handlers are counted even while intake is closed, allowing early
  /// arrivals to preserve a pending bit without touching the native pool. Epochs
  /// invalidate old registrations; resource-generation metadata independently
  /// rejects frames that do not match the prepared allocation. The main-thread
  /// owner changes allocation metadata only after admitted callbacks drain.
  /// The policy owns no pool or checkout and cannot prove external GPU retirement.
  class FrameCallbackPolicy final {
  public:
    /// Keeps callback-visible wake ownership live through intake and final wake.
    class Admission final {
    public:
      ~Admission();

      Admission(const Admission &) = delete;
      Admission &operator=(const Admission &) = delete;
      Admission(Admission &&other) noexcept;
      Admission &operator=(Admission &&) = delete;

      /// Recheck epoch and intake state after admission; stop may have raced entry.
      [[nodiscard]] bool is_open() const noexcept;
      /// Remember an arrival only while this admission still matches the epoch.
      void record_pending() noexcept;
      /// Complete the set/recheck half of the pending-arrival opening handshake.
      void wake_if_open() noexcept;

    private:
      friend class FrameCallbackPolicy;
      Admission(FrameCallbackPolicy &owner, std::uint64_t epoch) noexcept;

      FrameCallbackPolicy *owner_{};
      std::uint64_t epoch_{};
    };

    /// Construct closed admission with fixed pool count, format, and wake callable.
    FrameCallbackPolicy(std::uint32_t pool_buffer_count, core::Extent allocation, std::uint64_t resource_generation, CapturePixelFormat pool_format, std::function<void()> wake_main_loop);

    FrameCallbackPolicy(const FrameCallbackPolicy &) = delete;
    FrameCallbackPolicy &operator=(const FrameCallbackPolicy &) = delete;

    /// Registration epoch, distinct from the runtime's resource-generation tag.
    [[nodiscard]] std::uint64_t current_epoch() const noexcept;
    /// Whether one current-epoch arrival still needs bounded service.
    [[nodiscard]] bool arrival_pending() const noexcept;
    /// Whether matching callbacks may pull frames, not whether a guard may enter.
    [[nodiscard]] bool admission_open() const noexcept;
    /// Upper bound on main-thread pending-arrival pulls per service pass.
    [[nodiscard]] std::uint32_t pool_buffer_count() const noexcept;
    /// Count a matching nonterminal handler even when intake is temporarily closed.
    [[nodiscard]] std::optional<Admission>
    enter(std::uint64_t registered_epoch) noexcept;

    /// Publish open first, then observe the pending bit without clearing it.
    void open_current_epoch() noexcept;
    /// Main-thread consumer atomically takes pending work only while admitted.
    [[nodiscard]] bool claim_pending_arrival() noexcept;
    /// Wake on an empty-to-ready mailbox transition while caller admission lives.
    void notify_mailbox_ready() noexcept;

    /// @brief Validate allocation identity and retain the newest requested size.
    ///
    /// Content size may differ from allocation during a resize; that requests
    /// reconfiguration rather than authorizing an out-of-bounds copy. Generation,
    /// allocation, format, and invalid dimensions are fatal contract violations.
    /// Once fatal is latched, later frames cannot restore intake acceptance.
    [[nodiscard]] FrameObservationResult
    observe_frame(const FrameObservation &frame) noexcept;
    /// Retain a native projection failure as one sticky, allocation-free signal.
    void record_projection_failure(std::int64_t native_detail) noexcept;
    /// Contain allocation failure before leaving the WinRT delegate boundary.
    void record_out_of_memory() noexcept;
    /// Contain an otherwise unclassified exception with a fixed native detail.
    void record_unknown_callback_failure() noexcept;
    /// Clear notification edges while continuing to report sticky fatal state.
    [[nodiscard]] CaptureSignalBatch drain_signals() noexcept;

    /// Advance registration epoch and request final wake only if ever published.
    [[nodiscard]] std::uint64_t close_for_reconfigure() noexcept;
    /// A final wake remains in flight until its borrowed callable has returned.
    [[nodiscard]] bool frame_callbacks_quiescent() const noexcept;
    /// Replace allocation metadata only while closed, quiescent, and nonfatal.
    [[nodiscard]] std::expected<void, core::AppError>
    prepare_current_epoch(core::Extent allocation,
                          std::uint64_t resource_generation) noexcept;
    /// Permanently reject entry, discard pending resize/arrival, and advance epoch.
    void request_terminal_stop() noexcept;
    /// Terminal state is monotonic and survives every reconfiguration attempt.
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
    // Failed unpublished jobs have no tick owner and must never wake a dead host.
    bool ever_opened_{};
    bool quiescence_wake_requested_{};
    bool final_quiescence_wake_claimed_{};
    mutable ExclusiveSrwLock signal_lock_;
    std::optional<CaptureFatalSignal> fatal_;
    bool fatal_edge_{};
    bool resize_edge_{};
    std::optional<core::Extent> resize_;
  };

  /// @brief Separate fallible capture start from the committed publication state.
  ///
  /// A failed start must leave callback intake closed so partially constructed
  /// owners can unwind before a tick loop owns their lifetime. Successful native
  /// start commits the gate before invoking the supplied admission opener.
  /// @pre SourceClosed and OpenAdmission do not throw; the caller has completed
  /// all other fallible publication preparation before making its single attempt.
  class PublicationGate final {
  public:
    /// Publish only after StartCapture succeeds; earlier failures remain inactive.
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

    /// A started job requires explicit safe drain before ordinary destruction.
    [[nodiscard]] bool started() const noexcept { return started_; }

  private:
    bool attempted_{};
    bool started_{};
  };

  /// @brief Retain evidence of native cleanup independently of callback drain.
  ///
  /// Revocation and session close are attempted independently so failure of one
  /// does not skip the other. The first failure remains sticky; later calls do
  /// not retry or turn uncertainty into a successful stop proof. Pool and session
  /// owners must therefore remain alive while native cleanup is unverified.
  class CaptureJobStopState final {
  public:
    /// Attempt both cleanup steps once and contain every exception independently.
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

    /// True only when handler revocation and session close both completed.
    [[nodiscard]] bool native_stop_succeeded() const noexcept;
    /// Fixed retained diagnostic for runtime failure/drain classification.
    [[nodiscard]] std::optional<CaptureCleanupFailure>
    cleanup_failure() const noexcept;
    /// Combine native-stop proof with callback and mailbox completion facts.
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

  /// Map source closure and permission/native construction failures to port errors.
  [[nodiscard]] core::AppError
  classify_wgc_construction_failure(bool source_closed,
                                    std::int64_t native_detail,
                                    bool permission_denied = false) noexcept;

  /// Fail fast rather than unwind a published pool with uncertain internal users.
  void enforce_wgc_job_destruction_invariant(bool safe_to_destroy) noexcept;

}    // namespace avb::platform::windows::detail

#endif    // AVB0795_PLATFORM_WINDOWS_DETAIL_WGC_CAPTURE_STATE_HPP
