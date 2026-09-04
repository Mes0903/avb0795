#ifndef AIRELINNA_CORE_FRAME_SCHEDULER_HPP
#define AIRELINNA_CORE_FRAME_SCHEDULER_HPP

#include "airelinna/core/types.hpp"

#include <chrono>
#include <optional>

namespace airelinna::core {

struct FrameSchedule {
  bool render{};
  std::optional<std::chrono::steady_clock::time_point> next_wake;
};

class FrameScheduler final {
public:
  using Clock = std::chrono::steady_clock;

  FrameScheduler(FpsCap fps_cap, Clock::time_point now) noexcept;

  [[nodiscard]] FrameSchedule poll(Clock::time_point now, bool has_new_frame,
                                   bool redraw_requested) noexcept;

private:
  [[nodiscard]] static Clock::duration period_for(FpsCap fps_cap) noexcept;

  FpsCap fps_cap_;
  Clock::duration period_{};
  std::optional<Clock::time_point> next_deadline_;
  bool pending_work_{};
};

} // namespace airelinna::core

#endif // AIRELINNA_CORE_FRAME_SCHEDULER_HPP
