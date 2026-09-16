/// @file
/// @brief Capture admission epochs, bounded latches, publication, and stop proofs.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/detail/wgc_capture_state.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>

namespace avb::platform::windows::detail {

  namespace {

    constexpr auto maximum_native_extent =
      static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());

  }    // namespace

  core::Extent copyable_content_extent(const core::Extent content,
                                       const core::Extent allocation) noexcept
  {
    return {.width = std::min(content.width, allocation.width),
            .height = std::min(content.height, allocation.height)};
  }

  FrameCallbackPolicy::Admission::Admission(FrameCallbackPolicy &owner,
                                            const std::uint64_t epoch) noexcept
      : owner_{&owner}, epoch_{epoch} {}

  FrameCallbackPolicy::Admission::~Admission()
  {
    if (owner_ != nullptr) {
      owner_->leave_callback();
    }
  }

  FrameCallbackPolicy::Admission::Admission(Admission &&other) noexcept
      : owner_{std::exchange(other.owner_, nullptr)}, epoch_{other.epoch_} {}

  bool FrameCallbackPolicy::Admission::is_open() const noexcept
  {
    return owner_ != nullptr && owner_->epoch_matches(epoch_) &&
           owner_->admission_open();
  }

  void FrameCallbackPolicy::Admission::record_pending() noexcept
  {
    if (owner_ != nullptr) {
      owner_->record_pending(epoch_);
    }
  }

  void FrameCallbackPolicy::Admission::wake_if_open() noexcept
  {
    if (owner_ != nullptr) {
      owner_->wake_if_open(epoch_);
    }
  }

  FrameCallbackPolicy::FrameCallbackPolicy(
    const std::uint32_t pool_buffer_count, const core::Extent allocation, const std::uint64_t resource_generation, const CapturePixelFormat pool_format, std::function<void()> wake_main_loop)
      : pool_buffer_count_{pool_buffer_count}, allocation_{allocation}, resource_generation_{resource_generation}, pool_format_{pool_format}, wake_main_loop_{std::move(wake_main_loop)}
  {
    if (pool_buffer_count_ == 0 || allocation_.width == 0 ||
        allocation_.height == 0 || !wake_main_loop_) {
      throw std::invalid_argument{"invalid WGC callback policy"};
    }
  }

  std::uint64_t FrameCallbackPolicy::current_epoch() const noexcept
  {
    return epoch_.load(std::memory_order_acquire);
  }

  bool FrameCallbackPolicy::arrival_pending() const noexcept
  {
    return arrival_pending_.load(std::memory_order_seq_cst);
  }

  bool FrameCallbackPolicy::admission_open() const noexcept
  {
    return admission_open_.load(std::memory_order_seq_cst);
  }

  std::uint32_t FrameCallbackPolicy::pool_buffer_count() const noexcept
  {
    return pool_buffer_count_;
  }

  std::optional<FrameCallbackPolicy::Admission>
  FrameCallbackPolicy::enter(const std::uint64_t registered_epoch) noexcept
  {
    // Count matching handlers before rechecking whether native intake is open.
    const auto admission_guard = callback_admission_lock_.acquire();
    if (!epoch_matches(registered_epoch)) {
      return std::nullopt;
    }
    if (callbacks_in_flight_ == (std::numeric_limits<std::size_t>::max)()) {
      return std::nullopt;
    }
    ++callbacks_in_flight_;
    return std::optional<Admission>{Admission{*this, registered_epoch}};
  }

  void FrameCallbackPolicy::open_current_epoch() noexcept
  {
    {
      const auto admission_guard = callback_admission_lock_.acquire();
      if (terminal()) {
        return;
      }
      // Unpublished failures must never send a final borrowed-host wake during unwind.
      ever_opened_ = true;
      admission_open_.store(true, std::memory_order_seq_cst);
      if (callbacks_in_flight_ == 0) {
        quiescence_wake_requested_ = false;
        final_quiescence_wake_claimed_ = false;
      }
    }
    // Never clear here: callback set/recheck and opener store/read cover both races.
    if (arrival_pending()) {
      wake_noexcept();
    }
  }

  bool FrameCallbackPolicy::claim_pending_arrival() noexcept
  {
    return !terminal() && admission_open() &&
           arrival_pending_.exchange(false, std::memory_order_seq_cst);
  }

  void FrameCallbackPolicy::notify_mailbox_ready() noexcept { wake_noexcept(); }

  FrameObservationResult
  FrameCallbackPolicy::observe_frame(const FrameObservation &frame) noexcept
  {
    if (terminal()) {
      return {};
    }
    // Immutable allocation/generation/format contracts are separate from ContentSize.
    if (frame.resource_generation != resource_generation_ ||
        frame.allocation_extent != allocation_ || frame.content_size.width == 0 ||
        frame.content_size.height == 0 ||
        frame.content_size.width > maximum_native_extent ||
        frame.content_size.height > maximum_native_extent) {
      latch_fatal(CaptureFatalCategory::InvalidFrameMetadata, E_INVALIDARG);
      return {};
    }
    if (frame.surface_format != pool_format_) {
      latch_fatal(CaptureFatalCategory::FormatMismatch, 0);
      return {};
    }

    bool wake{};
    {
      const auto signal_guard = signal_lock_.acquire();
      // Terminal stop clears resize state under this same lock. Recheck here so
      // an already-admitted callback cannot restore it after that clear.
      if (terminal() || fatal_) {
        return {};
      }
      if (frame.content_size != allocation_) {
        resize_ = frame.content_size;
        if (!resize_edge_) {
          resize_edge_ = true;
          wake = true;
        }
      }
    }
    return {.accepted = true, .signal_edge = wake};
  }

  void FrameCallbackPolicy::record_projection_failure(
    const std::int64_t native_detail) noexcept
  {
    latch_fatal(CaptureFatalCategory::ProjectionFailure, native_detail);
  }

  void FrameCallbackPolicy::record_out_of_memory() noexcept
  {
    latch_fatal(CaptureFatalCategory::OutOfMemory, E_OUTOFMEMORY);
  }

  void FrameCallbackPolicy::record_unknown_callback_failure() noexcept
  {
    latch_fatal(CaptureFatalCategory::UnknownCallbackFailure, E_FAIL);
  }

  CaptureSignalBatch FrameCallbackPolicy::drain_signals() noexcept
  {
    const auto signal_guard = signal_lock_.acquire();
    CaptureSignalBatch batch{
      .fatal_active = fatal_.has_value(),
      .fatal_edge = std::exchange(fatal_edge_, false),
      .fatal = fatal_.value_or(CaptureFatalSignal{}),
      .resize = std::exchange(resize_, std::nullopt),
    };
    resize_edge_ = false;
    return batch;
  }

  std::uint64_t FrameCallbackPolicy::close_for_reconfigure() noexcept
  {
    const auto admission_guard = callback_admission_lock_.acquire();
    // A final wake belongs only to an owner that reached active publication.
    quiescence_wake_requested_ = ever_opened_;
    admission_open_.store(false, std::memory_order_seq_cst);
    arrival_pending_.store(false, std::memory_order_seq_cst);
    return epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
  }

  bool FrameCallbackPolicy::frame_callbacks_quiescent() const noexcept
  {
    const auto admission_guard = callback_admission_lock_.acquire();
    return callbacks_in_flight_ == 0;
  }

  std::expected<void, core::AppError> FrameCallbackPolicy::prepare_current_epoch(
    const core::Extent allocation,
    const std::uint64_t resource_generation) noexcept
  {
    const auto admission_guard = callback_admission_lock_.acquire();
    if (terminal() || admission_open() || callbacks_in_flight_ != 0 ||
        allocation.width == 0 || allocation.height == 0) {
      return std::unexpected{
        core::AppError{.code = core::AppErrorCode::InvalidCommand}};
    }
    {
      const auto signal_guard = signal_lock_.acquire();
      if (fatal_) {
        return std::unexpected{
          core::AppError{.code = core::AppErrorCode::InvalidCommand}};
      }
      // Closed admission and callback quiescence permit replacing generation metadata.
      allocation_ = allocation;
      resource_generation_ = resource_generation;
      resize_.reset();
      resize_edge_ = false;
    }
    arrival_pending_.store(false, std::memory_order_seq_cst);
    quiescence_wake_requested_ = false;
    final_quiescence_wake_claimed_ = false;
    return {};
  }

  void FrameCallbackPolicy::request_terminal_stop() noexcept
  {
    {
      const auto admission_guard = callback_admission_lock_.acquire();
      if (terminal_.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      admission_open_.store(false, std::memory_order_seq_cst);
      quiescence_wake_requested_ = ever_opened_;
      arrival_pending_.store(false, std::memory_order_seq_cst);
      epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    const auto signal_guard = signal_lock_.acquire();
    resize_.reset();
    resize_edge_ = false;
  }

  bool FrameCallbackPolicy::terminal() const noexcept
  {
    return terminal_.load(std::memory_order_acquire);
  }

  bool FrameCallbackPolicy::epoch_matches(
    const std::uint64_t epoch) const noexcept
  {
    return !terminal() && current_epoch() == epoch;
  }

  void FrameCallbackPolicy::leave_callback() noexcept
  {
    {
      const auto admission_guard = callback_admission_lock_.acquire();
      if (callbacks_in_flight_ == 0) {
        return;
      }
      if (callbacks_in_flight_ == 1 && quiescence_wake_requested_ &&
          !final_quiescence_wake_claimed_) {
        final_quiescence_wake_claimed_ = true;
      }
      else {
        --callbacks_in_flight_;
        return;
      }
    }

    // Keep admission counted while the borrowed target runs, outside the lock.
    wake_noexcept();
    const auto admission_guard = callback_admission_lock_.acquire();
    --callbacks_in_flight_;
  }

  void FrameCallbackPolicy::record_pending(const std::uint64_t epoch) noexcept
  {
    if (epoch_matches(epoch)) {
      arrival_pending_.store(true, std::memory_order_seq_cst);
    }
  }

  void FrameCallbackPolicy::wake_if_open(const std::uint64_t epoch) noexcept
  {
    if (epoch_matches(epoch) && admission_open()) {
      wake_noexcept();
    }
  }

  void FrameCallbackPolicy::latch_fatal(
    const CaptureFatalCategory category,
    const std::int64_t native_detail) noexcept
  {
    bool wake{};
    {
      const auto signal_guard = signal_lock_.acquire();
      if (!terminal() && !fatal_) {
        fatal_ = CaptureFatalSignal{category, native_detail};
        fatal_edge_ = true;
        resize_.reset();
        resize_edge_ = false;
        wake = true;
      }
    }
    if (wake) {
      wake_noexcept();
    }
  }

  void FrameCallbackPolicy::wake_noexcept() noexcept
  {
    try {
      wake_main_loop_();
    } catch (...) {
    }
  }

  bool CaptureJobStopState::native_stop_succeeded() const noexcept
  {
    return native_stop_succeeded_;
  }

  std::optional<CaptureCleanupFailure>
  CaptureJobStopState::cleanup_failure() const noexcept
  {
    return cleanup_failure_;
  }

  bool CaptureJobStopState::stop_quiescent(
    const bool callbacks_quiescent, const bool mailbox_closed) const noexcept
  {
    return requested_ && native_stop_succeeded_ && callbacks_quiescent &&
           mailbox_closed;
  }

  std::int64_t CaptureJobStopState::current_exception_detail() noexcept
  {
    try {
      throw;
    } catch (const winrt::hresult_error &error) {
      return error.code().value;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  void CaptureJobStopState::record_failure(
    const CaptureCleanupCategory category,
    const std::int64_t native_detail) noexcept
  {
    if (!cleanup_failure_) {
      cleanup_failure_ = CaptureCleanupFailure{category, native_detail};
    }
  }

  core::AppError
  classify_wgc_construction_failure(const bool source_closed,
                                    const std::int64_t native_detail,
                                    const bool permission_denied) noexcept
  {
    if (source_closed) {
      return {.code = core::AppErrorCode::SourceUnavailable,
              .native_detail = native_detail};
    }
    return {.code = permission_denied
                      ? core::AppErrorCode::PermissionDenied
                      : core::AppErrorCode::RuntimeConstructionFailure,
            .native_detail = native_detail};
  }

  void enforce_wgc_job_destruction_invariant(
    const bool safe_to_destroy) noexcept
  {
    if (!safe_to_destroy) {
      std::terminate();
    }
  }

}    // namespace avb::platform::windows::detail
