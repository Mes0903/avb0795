/// @file Demand-latched FPS policy driven by externally supplied steady-clock time.
#include "airelinna/core/frame_scheduler.hpp"

#include <cstdint>

namespace airelinna::core {

  FrameScheduler::FrameScheduler(FpsCap fps_cap, Clock::time_point now) noexcept
      : fps_cap_(fps_cap), period_(period_for(fps_cap))
  {
    if (fps_cap_ != FpsCap::Unlimited) {
      next_deadline_ = now;
    }
  }

  FrameSchedule FrameScheduler::poll(Clock::time_point now, bool has_new_frame, bool redraw_requested) noexcept
  {
    // A one-time arrival must survive later polls that report no new frame.
    pending_work_ = pending_work_ || has_new_frame || redraw_requested;
    if (!pending_work_) {
      return {};
    }

    if (fps_cap_ == FpsCap::Unlimited) {
      pending_work_ = false;
      return {.render = true, .next_wake = std::nullopt};
    }

    if (now < *next_deadline_) {
      return {.render = false, .next_wake = next_deadline_};
    }

    // Preserve the deadline grid but skip every elapsed slot: render newest work
    // once, never build a catch-up burst after a delayed main-thread tick.
    const auto missed_periods = ((now - *next_deadline_) / period_) + 1;
    *next_deadline_ += period_ * missed_periods;
    pending_work_ = false;

    return {.render = true, .next_wake = std::nullopt};
  }

  FrameScheduler::Clock::duration
  FrameScheduler::period_for(FpsCap fps_cap) noexcept
  {
    std::uint32_t frames_per_second{};
    switch (fps_cap) {
    case FpsCap::Fps30:
      frames_per_second = 30;
      break;
    case FpsCap::Fps60:
      frames_per_second = 60;
      break;
    case FpsCap::Fps144:
      frames_per_second = 144;
      break;
    case FpsCap::Unlimited:
      return {};
    }

    return std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>{1.0 /
                                    static_cast<double>(frames_per_second)});
  }

}    // namespace airelinna::core
