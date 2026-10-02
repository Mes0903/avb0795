/// @file
/// @brief Clock-driven frame admission without sleeps or catch-up rendering.
#ifndef AVB0795_CORE_FRAME_SCHEDULER_HPP
#define AVB0795_CORE_FRAME_SCHEDULER_HPP

#include "avb/core/types.hpp"

#include <chrono>
#include <optional>

namespace avb::core {

  /// @brief One admission decision, not evidence that a frame was rendered or presented.
  /// next_wake is present only when pending work was deferred by a cap. An empty
  /// deadline asks the caller to rely on new events rather than periodic polling.
  struct FrameSchedule {
    bool render{};
    std::optional<std::chrono::steady_clock::time_point> next_wake;
  };

  /// @brief Coalesce frame and redraw demand on a fixed deadline grid.
  /// The caller supplies monotonic time and owns rendering, frame storage and waits.
  /// No work is queued here beyond a demand bit; reconstruct the scheduler to change
  /// the cap. Calls are confined to one thread or require external serialization.
  class FrameScheduler final {
  public:
    using Clock = std::chrono::steady_clock;

    /// @brief Anchor capped deadlines at now so initial work can render immediately.
    /// @pre fps_cap is a declared enumerator; subsequent poll times use this clock.
    FrameScheduler(FpsCap fps_cap, Clock::time_point now) noexcept;

    /// @brief Latch either demand signal until eligible, then consume it in one admission.
    /// Repeated signals coalesce while waiting. Admission advances past all elapsed
    /// grid slots, so a delayed tick never creates a catch-up burst. The grid is
    /// anchored at construction, not at the last admission; this does not promise
    /// a full frame period between two admissions near a grid boundary.
    /// Idle and Unlimited polls never create a periodic wake without pending work.
    /// @pre now does not move backwards relative to previous polls.
    [[nodiscard]] FrameSchedule poll(Clock::time_point now, bool has_new_frame, bool redraw_requested) noexcept;

  private:
    [[nodiscard]] static Clock::duration period_for(FpsCap fps_cap) noexcept;

    FpsCap fps_cap_;
    Clock::duration period_{};
    std::optional<Clock::time_point> next_deadline_;
    bool pending_work_{};
  };

}    // namespace avb::core

#endif    // AVB0795_CORE_FRAME_SCHEDULER_HPP
