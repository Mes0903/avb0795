/// @file
/// @brief Explicit acquisition admission, identity arbitration and shutdown retirement.
#include "avb/app/application.hpp"
#include "avb/app/ports.hpp"
#include <limits>
#include <type_traits>

namespace avb::app {
  void Application::start_acquisition(core::SourceKind kind, bool fresh, Clock::time_point now)
  {
    if (!source_supported(kind) || !selection_allowed() || acquisition_request_ || pending_start_) {
      reject();
      return;
    }
    chosen_source_kind_ = kind;
    if (runtime_) {
      const auto &session = state_.current_session();
      pending_start_ = PendingStart{.source = *session.selected_source(),
                                    .settings = session.settings(),
                                    .acquisition = true,
                                    .fresh = fresh,
                                    .kind = kind};
      request_stop(false, now, false);
      return;
    }
    if (last_acquisition_id_ == std::numeric_limits<std::uint64_t>::max()) {
      reject(core::AppError{core::AppErrorCode::InvalidValue});
      return;
    }
    SourceAcquisitionRequest request{.id = {++last_acquisition_id_},
                                     .session_id = state_.current_session_id(),
                                     .kind = kind};
    const auto &selected = state_.current_session().selected_source();
    if (!fresh && selected && selected->kind == kind) request.resume_source = selected->id;
    // Publication authority is retained before native admission, including typed failure.
    acquisition_request_ = request;
    acquisition_start_intent_ = true;
    acquisition_quiescent_ = false;
    acquisition_phase_ = request.resume_source ? AcquisitionPhase::PreparingStream
                                               : AcquisitionPhase::AwaitingPermission;
    auto begun = source_acquisition_->begin(request);
    if (!begun) {
      acquisition_request_.reset();
      acquisition_start_intent_ = false;
      acquisition_phase_ = AcquisitionPhase::Idle;
      reject(begun.error());
    }
    else
      clear_notice(NoticeOperation::Command);
  }

  void Application::cancel_acquisition() noexcept
  {
    if (!acquisition_request_ || !acquisition_start_intent_) return;
    // Revoke intent first so a queued Ready in this step cannot publish a runtime.
    acquisition_start_intent_ = false;
    acquisition_phase_ = AcquisitionPhase::Cancelling;
    source_acquisition_->cancel(acquisition_request_->id);
  }

  void Application::tick_acquisition(Clock::time_point now)
  {
    if (!source_acquisition_) return;
    auto result = source_acquisition_->tick(now);
    acquisition_deadline_ = result.next_wake;
    acquisition_quiescent_ = result.quiescent;
    if (quit_intent_) {
      if (acquisition_quiescent_) {
        // Service retirement closes its clock even when runtime obligations remain.
        // Runtime reports its own progress-based stall authority through status facts.
        acquisition_shutdown_deadline_.reset();
        acquisition_deadline_.reset();
      } else if (result.made_progress) {
        acquisition_shutdown_deadline_ = now + std::chrono::seconds{10};
      }
    }
    for (auto &event : result.events) {
      if (!event) continue;
      std::visit([&](auto &fact) {
        if (!acquisition_request_ || fact.id != acquisition_request_->id) return;
        using Fact = std::decay_t<decltype(fact)>;
        if constexpr (std::is_same_v<Fact, AcquisitionPhaseChanged>) {
          if (acquisition_start_intent_ && !quit_intent_ &&
              (fact.phase == AcquisitionPhase::AwaitingPermission ||
               fact.phase == AcquisitionPhase::PreparingStream)) acquisition_phase_ = fact.phase;
        }
        else {
          if constexpr (std::is_same_v<Fact, AcquisitionReady>) {
            if (!acquisition_start_intent_ || quit_intent_) return;
          }
          const auto request = *acquisition_request_;
          const bool authorized = acquisition_start_intent_ && !quit_intent_ &&
                                  request.session_id == state_.current_session_id() && !runtime_ && !stop_requested_;
          acquisition_request_.reset();
          acquisition_start_intent_ = false;
          acquisition_phase_ = AcquisitionPhase::Idle;
          if constexpr (std::is_same_v<Fact, AcquisitionReady>) {
            if (!authorized) return;
            if (fact.source.kind != request.kind || !source_supported(fact.source.kind)) {
              reject(core::AppError{core::AppErrorCode::InvalidValue});
              return;
            }
            const auto &selected = state_.current_session().selected_source();
            // Identity, not discovery extent, preserves a source-local accepted crop.
            if (!selected || selected->id != fact.source.id) {
              auto selection = state_.select_source(request.session_id, fact.source);
              if (!selection) {
                reject(selection.error());
                return;
              }
              invalidate_region_interaction();
            }
            start_current(now, false);
          }
          else if constexpr (std::is_same_v<Fact, AcquisitionFailed>) {
            if (authorized) reject(fact.error);
          }
          else if (authorized)
            clear_notice(NoticeOperation::Command);
        }
      },
                 *event);
    }
  }

  void Application::evaluate_shutdown(Clock::time_point now) noexcept
  {
    if (!quit_intent_ || exit_disposition_ != core::ExitDisposition::Continue) return;
    // The shutdown step already ticked native work; do not retain its consumed immediate wake.
    if (source_acquisition_ && immediate_deadline_ && *immediate_deadline_ <= now) immediate_deadline_.reset();
    if (!runtime_ && (!source_acquisition_ || acquisition_quiescent_)) {
      exit_disposition_ = core::ExitDisposition::Clean;
      acquisition_shutdown_deadline_.reset();
      acquisition_deadline_.reset();
    }
    else if (!acquisition_quiescent_ && acquisition_shutdown_deadline_ &&
             now >= *acquisition_shutdown_deadline_) {
      // Only unresolved service ownership is timed here. Runtime's own watchdog
      // accounts for intermediate retirement that does not emit a status edge.
      // Timing out cannot grant permission to release either uncertain owner.
      emergency_abandon();
    }
  }
}    // namespace avb::app
