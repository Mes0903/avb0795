#ifndef AIRELINNA_PLATFORM_WINDOWS_DETAIL_WINDOWS_RUNTIME_POLICY_HPP
#define AIRELINNA_PLATFORM_WINDOWS_DETAIL_WINDOWS_RUNTIME_POLICY_HPP

#include "platform/windows/wgc_capture_job.hpp"
#include "render/bridge_policy.hpp"
#include "render/graphics_retry_policy.hpp"

#include <chrono>
#include <optional>

namespace airelinna::platform::windows::detail {

  struct RuntimeBridgeEventState final {
    std::optional<render::BridgeSelection> published;
    std::optional<render::BridgeSelection> pending;
  };

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

  [[nodiscard]] constexpr RuntimeBridgeEventState
  commit_runtime_bridge_selection(RuntimeBridgeEventState state) noexcept
  {
    if (state.pending) {
      state.published = state.pending;
      state.pending.reset();
    }
    return state;
  }

  struct RuntimeRecoveryEventState final {
    bool published_active{};
    bool pending_started{};
    bool pending_completed{};
  };

  struct RuntimeRecoveryObservations final {
    bool started{};
    bool completed{};
    bool completion_superseded{};
  };

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

  struct RuntimeCaptureFacts final {
    bool source_closed{};
    bool capture_failed{};
    std::optional<core::Extent> resize;
  };

  [[nodiscard]] constexpr bool capture_cleanup_is_uncertain(
    const std::optional<CaptureCleanupFailure> &failure) noexcept
  {
    return failure.has_value();
  }

  [[nodiscard]] constexpr RuntimeCaptureFacts
  merge_runtime_capture_signals(const bool source_closed,
                                const CaptureSignalBatch &signals) noexcept
  {
    return {.source_closed = source_closed,
            .capture_failed = signals.fatal_active,
            .resize = signals.resize};
  }

  enum class SourceReconfigurationPhase {
    Inactive,
    WaitingForCallbacks,
    WaitingForRender,
    Prepared,
  };

  enum class SourceReconfigurationAction {
    None,
    Wait,
    DiscardFinalFrame,
    Prepare,
    Commit,
    Cancel,
  };

  struct SourceReconfigurationFacts final {
    SourceReconfigurationPhase phase{SourceReconfigurationPhase::Inactive};
    bool callbacks_quiescent{};
    bool render_idle{};
    bool source_open{true};
    bool capture_failed{};
  };

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

  struct RuntimeDrainState final {
    std::optional<std::chrono::steady_clock::time_point> no_progress_since;
    core::RuntimeDrainStatus published_status{core::RuntimeDrainStatus::Normal};
    bool ownership_uncertain{};
  };

  struct RuntimeDrainUpdate final {
    RuntimeDrainState state;
    std::optional<core::RuntimeDrainStatus> status_change;
    bool may_complete{};
    std::optional<std::chrono::steady_clock::time_point> next_deadline;
  };

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

  enum class RuntimeExceptionPhase { Running,
                                     Stopping };

  struct RuntimeExceptionObservation final {
    bool emit_failure{};
    bool retain_owners{true};
    RuntimeDrainUpdate drain;
  };

  [[nodiscard]] inline RuntimeExceptionObservation observe_runtime_exception(
    RuntimeDrainState state, const RuntimeExceptionPhase phase, const std::chrono::steady_clock::time_point now) noexcept
  {
    const auto drain = advance_runtime_drain(state, true, true,
                                             render::DrainProgress::Retry, now);
    return {.emit_failure = phase == RuntimeExceptionPhase::Running,
            .retain_owners = true,
            .drain = drain};
  }

}    // namespace airelinna::platform::windows::detail

#endif    // AIRELINNA_PLATFORM_WINDOWS_DETAIL_WINDOWS_RUNTIME_POLICY_HPP
