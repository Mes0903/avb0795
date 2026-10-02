#include "avb/core/frame_scheduler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

TEST_CASE("avb0795 V1 case 03: FPS scheduler caps and missed-period skip",
          "[avb0795-v1]")
{
  using avb::core::FpsCap;
  using avb::core::FrameScheduler;
  using Clock = FrameScheduler::Clock;

  struct Row {
    FpsCap cap;
    std::optional<std::uint32_t> frames_per_second;
  };

  constexpr std::array rows{
    Row{FpsCap::Fps30, 30},
    Row{FpsCap::Fps60, 60},
    Row{FpsCap::Fps144, 144},
    Row{FpsCap::Unlimited, std::nullopt},
  };
  constexpr auto origin = Clock::time_point{};

  SECTION("all caps are event-driven and capped deadlines are monotonic")
  {
    for (const auto &row : rows) {
      CAPTURE(row.frames_per_second);
      FrameScheduler scheduler{row.cap, origin};

      const auto idle = scheduler.poll(origin, false, false);
      CHECK_FALSE(idle.render);
      CHECK_FALSE(idle.next_wake.has_value());

      const auto first_frame = scheduler.poll(origin, true, false);
      CHECK(first_frame.render);
      CHECK_FALSE(first_frame.next_wake.has_value());

      if (!row.frames_per_second) {
        const auto idle_again = scheduler.poll(origin, false, false);
        CHECK_FALSE(idle_again.render);
        CHECK_FALSE(idle_again.next_wake.has_value());

        const auto redraw = scheduler.poll(origin, false, true);
        CHECK(redraw.render);
        CHECK_FALSE(redraw.next_wake.has_value());
        continue;
      }

      const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>{
          1.0 / static_cast<double>(*row.frames_per_second)});
      const auto early =
        scheduler.poll(origin + period - Clock::duration{1}, true, false);
      CHECK_FALSE(early.render);
      CHECK(early.next_wake == origin + period);

      const auto due = scheduler.poll(origin + period, false, false);
      REQUIRE(due.render);
      CHECK_FALSE(due.next_wake.has_value());

      const auto next_early =
        scheduler.poll(origin + period + Clock::duration{1}, true, false);
      CHECK_FALSE(next_early.render);
      REQUIRE(next_early.next_wake == origin + (period * 2));
      CHECK(*next_early.next_wake > *early.next_wake);
    }
  }

  SECTION("one missed-deadline jump renders once and skips catch-up")
  {
    constexpr auto cap = FpsCap::Fps60;
    constexpr std::uint32_t frames_per_second{60};
    const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>{1.0 /
                                    static_cast<double>(frames_per_second)});
    FrameScheduler scheduler{cap, origin};

    REQUIRE(scheduler.poll(origin, true, false).render);
    const auto jumped_now = origin + (period * 5) + Clock::duration{1};
    const auto after_jump = scheduler.poll(jumped_now, true, false);
    CHECK(after_jump.render);
    CHECK_FALSE(after_jump.next_wake.has_value());

    const auto no_burst = scheduler.poll(jumped_now, true, true);
    CHECK_FALSE(no_burst.render);
    REQUIRE(no_burst.next_wake.has_value());
    CHECK(*no_burst.next_wake > jumped_now);
  }
}
