/// @file
/// @brief Value-only runtime event coalescing, resize barriers, and drain observation.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_WINDOWS_RUNTIME_POLICY_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_WINDOWS_RUNTIME_POLICY_HPP

#include "platform/windows/wgc_capture_job.hpp"
#include "platform/windows/detail/bridge_policy.hpp"
#include "render/graphics_retry_policy.hpp"

#include <chrono>
#include <optional>

namespace avb::platform::windows::detail {

  /// @brief Spend one frame admission on either a fresh copy or retained redraw.
  /// A newly adopted capture will render the latest presentation when its copy
  /// completes. Grant retained replay when dirty work has no such adopted frame.
  [[nodiscard]] constexpr bool admit_retained_redraw(const bool dirty,
                                                    const bool submitted_capture) noexcept
  {
    return dirty && !submitted_capture;
  }

  /// Published/pending bridge selection, so unchanged status does not emit duplicate events.
  struct RuntimeBridgeEventState final {
    std::optional<render::BridgeSelection> published;
    std::optional<render::BridgeSelection> pending;
  };

  /// Coalesce an observed selection against the last published value.
  [[nodiscard]] constexpr RuntimeBridgeEventState
  observe_runtime_bridge_selection(
    RuntimeBridgeEventState state,
    const std::optional<render::BridgeSelection> observed) noexcept
  {
    if (!observed) {
      return state;
    }
    state.pending = state.published == observed ? std::nullopt : observed;
    return state;
  }

  /// Commit only after the runtime actually adds the pending event to its result.
  [[nodiscard]] constexpr RuntimeBridgeEventState
  commit_runtime_bridge_selection(RuntimeBridgeEventState state) noexcept
  {
    if (state.pending) {
      state.published = state.pending;
      state.pending.reset();
    }
    return state;
  }

  /// Preserve causal started/completed facts until a runtime tick publishes them.
  struct RuntimeRecoveryEventState final {
    bool published_active{};
    bool pending_started{};
    bool pending_completed{};
  };

  /// Native recovery observations, including a newer episode superseding completion.
  struct RuntimeRecoveryObservations final {
    bool started{};
    bool completed{};
    bool completion_superseded{};
  };

  /// Coalesce edges without deciding portable lifecycle priority.
  [[nodiscard]] constexpr RuntimeRecoveryEventState
  observe_runtime_recovery_events(
    RuntimeRecoveryEventState state,
    const RuntimeRecoveryObservations observations) noexcept
  {
    if (observations.completion_superseded || observations.started) {
      state.pending_completed = false;
    }
    if (observations.started && !state.published_active) {
      state.pending_started = true;
    }
    if (observations.completed && !observations.completion_superseded &&
        (state.published_active || state.pending_started)) {
      state.pending_completed = true;
    }
    return state;
  }

  /// Update publication state after the event batch has accepted the causal pair.
  [[nodiscard]] constexpr RuntimeRecoveryEventState
  commit_runtime_recovery_events(RuntimeRecoveryEventState state) noexcept
  {
    if (state.pending_started) {
      state.published_active = true;
    }
    if (state.pending_completed) {
      state.published_active = false;
    }
    state.pending_started = false;
    state.pending_completed = false;
    return state;
  }

  /// Synchronous capture facts for the runtime's native stop/resize decisions.
  struct RuntimeCaptureFacts final {
    bool source_closed{};
    bool capture_failed{};
    std::optional<core::Extent> resize;
  };

  /// A retained revoke/Close failure forbids claiming safe native teardown.
  [[nodiscard]] constexpr bool capture_cleanup_is_uncertain(
    const std::optional<CaptureCleanupFailure> &failure) noexcept
  {
    return failure.has_value();
  }

  /// Combine sticky source and job facts without exposing callback-owned resources.
  [[nodiscard]] constexpr RuntimeCaptureFacts
  merge_runtime_capture_signals(const bool source_closed,
                                const CaptureSignalBatch &signals) noexcept
  {
    return {.source_closed = source_closed,
            .capture_failed = signals.fatal_active,
            .resize = signals.resize};
  }

  /// Main-thread barrier phases separating callback retirement from render retirement.
  enum class SourceReconfigurationPhase {
    Inactive,
    WaitingForCallbacks,
    WaitingForRender,
    Prepared,
  };

  /// Next concrete action; the policy itself never calls WGC or renderer APIs.
  enum class SourceReconfigurationAction {
    None,
    Wait,
    DiscardFinalFrame,
    Prepare,
    Commit,
    Cancel,
  };

  /// Inputs needed to reject resize on source loss/fatal and preserve owner ordering.
  struct SourceReconfigurationFacts final {
    SourceReconfigurationPhase phase{SourceReconfigurationPhase::Inactive};
    bool callbacks_quiescent{};
    bool render_idle{};
    bool source_open{true};
    bool capture_failed{};
  };

  /// @brief Order resize retirement before replacing the capture allocation.
  ///
  /// Callback quiescence allows discarding the final mailbox frame; graphics
  /// idleness then accounts for checkouts already transferred out of that mailbox.
  /// Source closure or fatal capture cancels every active phase, including a
  /// prepared replacement that has not yet reopened admission.
  [[nodiscard]] constexpr SourceReconfigurationAction
  next_source_reconfiguration_action(
    const SourceReconfigurationFacts facts) noexcept
  {
    if (facts.phase == SourceReconfigurationPhase::Inactive) {
      return SourceReconfigurationAction::None;
    }
    if (!facts.source_open || facts.capture_failed) {
      return SourceReconfigurationAction::Cancel;
    }
    switch (facts.phase) {
    case SourceReconfigurationPhase::Inactive:
      return SourceReconfigurationAction::None;
    case SourceReconfigurationPhase::WaitingForCallbacks:
      return facts.callbacks_quiescent
               ? SourceReconfigurationAction::DiscardFinalFrame
               : SourceReconfigurationAction::Wait;
    case SourceReconfigurationPhase::WaitingForRender:
      return facts.render_idle ? SourceReconfigurationAction::Prepare
                               : SourceReconfigurationAction::Wait;
    case SourceReconfigurationPhase::Prepared:
      return SourceReconfigurationAction::Commit;
    }
    return SourceReconfigurationAction::Cancel;
  }

  /// One aggregate no-progress clock for all retained native owners, not just graphics.
  struct RuntimeDrainState final {
    std::optional<std::chrono::steady_clock::time_point> no_progress_since;
    core::RuntimeDrainStatus published_status{core::RuntimeDrainStatus::Normal};
    bool ownership_uncertain{};
  };

  /// Drain status edge, release permission, and the next bounded observation deadline.
  struct RuntimeDrainUpdate final {
    RuntimeDrainState state;
    std::optional<core::RuntimeDrainStatus> status_change;
    bool may_complete{};
    std::optional<std::chrono::steady_clock::time_point> next_deadline;
  };

  /// @brief Track stalled cleanup without converting elapsed time into release permission.
  ///
  /// Polling or retrying does not reset the no-progress clock; tracked retirement
  /// and proven safe release do. Uncertainty remains sticky until an empty-owner
  /// observation explicitly proves release, so a later successful poll cannot
  /// erase an earlier loss of ownership evidence. Returned deadlines request
  /// another observation and do not imply that cleanup will complete by then.
  [[nodiscard]] inline RuntimeDrainUpdate advance_runtime_drain(
    RuntimeDrainState state, const bool pending_ownership, const bool ownership_uncertain, const render::DrainProgress progress, const std::chrono::steady_clock::time_point now) noexcept
  {
    state.ownership_uncertain = state.ownership_uncertain || ownership_uncertain;
    if (!pending_ownership && !ownership_uncertain &&
        progress == render::DrainProgress::ProvenSafeRelease) {
      state.ownership_uncertain = false;
    }
    const bool work_pending = pending_ownership || state.ownership_uncertain;
    if (!work_pending) {
      const auto changed =
        state.published_status != core::RuntimeDrainStatus::Normal
          ? std::optional{core::RuntimeDrainStatus::Normal}
          : std::nullopt;
      state.published_status = core::RuntimeDrainStatus::Normal;
      state.no_progress_since.reset();
      return {.state = state, .status_change = changed, .may_complete = true};
    }

    if (!state.no_progress_since ||
        progress == render::DrainProgress::TrackedRetirement ||
        progress == render::DrainProgress::ProvenSafeRelease) {
      state.no_progress_since = now;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - *state.no_progress_since);
    const render::RuntimeDrainPolicy policy;
    const auto decision =
      policy.evaluate({.pending_ownership = true,
                       .ownership_uncertain = state.ownership_uncertain,
                       .progress = progress,
                       .elapsed_no_progress = elapsed});
    const auto changed = decision.status != state.published_status
                           ? std::optional{decision.status}
                           : std::nullopt;
    state.published_status = decision.status;
    return {.state = state,
            .status_change = changed,
            .may_complete = decision.may_complete,
            .next_deadline = decision.poll_after
                               ? std::optional{now + *decision.poll_after}
                               : std::nullopt};
  }

  /// Exceptions while stopping must not reintroduce pre-stop lifecycle failures.
  enum class RuntimeExceptionPhase { Running,
                                     Stopping };

  /// No-throw fallback retaining owners and keeping future drain observation scheduled.
  struct RuntimeExceptionObservation final {
    bool emit_failure{};
    bool retain_owners{true};
    RuntimeDrainUpdate drain;
  };

  /// Convert an unexpected exception to existing failure/drain facts without allocation.
  [[nodiscard]] inline RuntimeExceptionObservation observe_runtime_exception(
    RuntimeDrainState state, const RuntimeExceptionPhase phase, const std::chrono::steady_clock::time_point now) noexcept
  {
    const auto drain = advance_runtime_drain(state, true, true,
                                             render::DrainProgress::Retry, now);
    return {.emit_failure = phase == RuntimeExceptionPhase::Running,
            .retain_owners = true,
            .drain = drain};
  }

}    // namespace avb::platform::windows::detail

#endif    // AVB0795_PLATFORM_WINDOWS_DETAIL_WINDOWS_RUNTIME_POLICY_HPP
