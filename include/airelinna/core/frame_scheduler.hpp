/// @file Clock-driven frame admission without sleeps or catch-up rendering.
#ifndef AIRELINNA_CORE_FRAME_SCHEDULER_HPP
#define AIRELINNA_CORE_FRAME_SCHEDULER_HPP

#include "airelinna/core/types.hpp"

#include <chrono>
#include <optional>

namespace airelinna::core {

  /// One polling decision; next_wake is present only for deferred pending work.
  struct FrameSchedule {
    bool render{};
    std::optional<std::chrono::steady_clock::time_point> next_wake;
  };

  /// Coalesces frame/redraw demand under a fixed cap on the caller's thread.
  /// The caller supplies monotonic time and performs rendering/waiting itself.
  class FrameScheduler final {
  public:
    using Clock = std::chrono::steady_clock;

    /// Anchor capped deadlines at now so initial work can render immediately.
    FrameScheduler(FpsCap fps_cap, Clock::time_point now) noexcept;

    /// Latch demand until eligible, then admit one render and skip missed periods.
    /// Idle and Unlimited polls never create a periodic wake without pending work.
    [[nodiscard]] FrameSchedule poll(Clock::time_point now, bool has_new_frame, bool redraw_requested) noexcept;

  private:
    [[nodiscard]] static Clock::duration period_for(FpsCap fps_cap) noexcept;

    FpsCap fps_cap_;
    Clock::duration period_{};
    std::optional<Clock::time_point> next_deadline_;
    bool pending_work_{};
  };

}    // namespace airelinna::core

#endif    // AIRELINNA_CORE_FRAME_SCHEDULER_HPP
