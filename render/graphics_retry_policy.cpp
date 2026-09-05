#include "render/graphics_retry_policy.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace airelinna::render {

  namespace {

    using namespace std::chrono_literals;

    constexpr std::array retry_deadlines{0ms, 100ms, 500ms};
    constexpr auto ownership_poll_interval = 50ms;
    constexpr auto stalled_threshold = 10s;

    [[nodiscard]] constexpr bool
    is_drain_progress(const DrainProgress progress) noexcept
    {
      return progress == DrainProgress::TrackedRetirement ||
             progress == DrainProgress::ProvenSafeRelease;
    }

  }    // namespace

  GraphicsRetryDecision
  GraphicsRetryPolicy::evaluate(const GraphicsRetryInput input) const noexcept
  {
    if (input.category == GraphicsFailureCategory::Terminal) {
      return {};
    }
    if (input.attempt >= retry_deadlines.size()) {
      return {};
    }

    auto action = GraphicsRetryAction::Retry;
    auto candidate_generation = input.current_generation;
    switch (input.category) {
    case GraphicsFailureCategory::Swapchain:
      action = GraphicsRetryAction::Retry;
      break;
    case GraphicsFailureCategory::Surface:
      action = GraphicsRetryAction::SurfaceRecreate;
      break;
    case GraphicsFailureCategory::Device:
      if (input.current_generation ==
          (std::numeric_limits<std::uint64_t>::max)()) {
        return {.action = GraphicsRetryAction::Terminal,
                .candidate_generation = std::nullopt};
      }
      action = GraphicsRetryAction::DeviceRebuild;
      candidate_generation = input.current_generation + 1;
      break;
    case GraphicsFailureCategory::Terminal:
      return {};
    }

    const auto elapsed = std::max(input.elapsed_no_progress, 0ms);
    const auto deadline = retry_deadlines[input.attempt];
    return {.action = action,
            .candidate_generation = candidate_generation,
            .retry_after = deadline > elapsed ? deadline - elapsed : 0ms};
  }

  RuntimeDrainDecision
  RuntimeDrainPolicy::evaluate(const RuntimeDrainInput input) const noexcept
  {
    if (!input.pending_ownership && !input.ownership_uncertain) {
      return {.status = core::RuntimeDrainStatus::Normal, .may_complete = true};
    }

    if (is_drain_progress(input.progress)) {
      return {.status = core::RuntimeDrainStatus::Normal,
              .restart_no_progress_interval = true,
              .poll_after = ownership_poll_interval};
    }

    const auto elapsed = std::max(input.elapsed_no_progress, 0ms);
    if (elapsed >= stalled_threshold) {
      return {.status = core::RuntimeDrainStatus::Stalled,
              .poll_after = ownership_poll_interval};
    }

    return {.status = core::RuntimeDrainStatus::Normal,
            .poll_after =
              std::min(ownership_poll_interval, stalled_threshold - elapsed)};
  }

}    // namespace airelinna::render
