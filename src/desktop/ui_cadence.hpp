/// @file
/// @brief Main-thread UI deadlines and host wait policy with a caller-owned clock.
#ifndef AVB0795_DESKTOP_UI_CADENCE_HPP
#define AVB0795_DESKTOP_UI_CADENCE_HPP
#include <chrono>
#include <initializer_list>
#include <optional>

namespace avb::desktop::detail {

  using HostTime = std::chrono::steady_clock::time_point;

  /// @brief Select an ordinary widget frame or an event-driven hidden input drain.
  enum class UiTickMode { None, Visible, HiddenSync };

  /// @brief A due action and the next observation time for the host's event wait.
  /// A due action carries the current deadline until did_build acknowledges it.
  /// Hidden, synchronized state contributes an empty deadline to the host wait.
  struct UiDecision {
    UiTickMode mode{};
    std::optional<HostTime> next_deadline;
  };

  /// @brief Bound widget rebuilding while application and graphics progress independently.
  ///
  /// Input observers extend a 500 ms activity grace; held-input, editing and popup
  /// facts can extend active cadence beyond that grace. Build completion establishes
  /// a fresh period, so a delayed host iteration skips missed ticks. Visibility
  /// transitions preserve pending input until one hidden synchronization frame
  /// consumes it, and restoration requests an immediate ordinary frame.
  /// @pre All calls belong to the host thread and use its monotonic clock.
  class UiCadence final {
  public:
    static constexpr auto active_period = std::chrono::nanoseconds{16'666'667};
    static constexpr auto idle_period = std::chrono::milliseconds{100};
    static constexpr auto activity_grace = std::chrono::milliseconds{500};

    /// @brief Record a real input or native display event before choosing a frame.
    /// Keeping the dirty bit while visible covers input followed by iconify in
    /// the same event-pump batch. Capture/retirement wakes query decision directly.
    void note_input(const HostTime now) noexcept
    {
      active_until_ = now + activity_grace;
      input_dirty_ = true;
    }

    /// @brief Acknowledge the input consumed by the just-completed frame.
    /// Call before actions such as native iconify that may synchronously emit new
    /// input callbacks; those later callbacks must survive for the following tick.
    void did_build(const HostTime now) noexcept
    {
      input_dirty_ = false;
      first_or_restore_ = false;
      if (visible_) last_build_ = now;
    }

    /// @brief Observe drawable visibility, including native iconification.
    /// A transition to hidden requests one synchronization even when the native
    /// backend queued its release/focus events before this visibility observation.
    void set_visible(const bool visible, const HostTime now) noexcept
    {
      if (visible_ == visible) return;
      visible_ = visible;
      if (visible) {
        first_or_restore_ = true;
        active_until_ = now + activity_grace;
      } else {
        input_dirty_ = true;
      }
    }

    /// @brief Derive the next UI action while preserving independent host deadlines.
    [[nodiscard]] UiDecision decision(const HostTime now,
                                      const bool interaction_active = false) const noexcept
    {
      if (!visible_) {
        return input_dirty_ ? UiDecision{UiTickMode::HiddenSync, now} : UiDecision{};
      }
      if (first_or_restore_ || !last_build_) return {UiTickMode::Visible, now};
      const auto period = interaction_active || now < active_until_
        ? active_period : std::chrono::duration_cast<std::chrono::nanoseconds>(idle_period);
      const auto due = *last_build_ + period;
      return {now >= due ? UiTickMode::Visible : UiTickMode::None, due};
    }

  private:
    std::optional<HostTime> last_build_;
    HostTime active_until_{};
    bool visible_{true};
    bool first_or_restore_{true};
    bool input_dirty_{};
  };

  /// @brief Distinguish immediate command delivery from a timed or indefinite wait.
  struct HostWaitDecision {
    bool immediate_reentry{};
    std::optional<HostTime> deadline;
  };

  /// @brief Preserve queued UI commands and choose the earliest independent owner deadline.
  /// A command emitted after Application::step requests a new pumped iteration.
  /// Otherwise an empty result permits event waiting; an overdue timestamp is
  /// handled by the host's existing immediate-return wait path.
  [[nodiscard]] inline HostWaitDecision choose_host_wait(
    const bool command_emitted, const std::optional<HostTime> application,
    const std::optional<HostTime> graphics, const std::optional<HostTime> ui) noexcept
  {
    if (command_emitted) return {true, std::nullopt};
    std::optional<HostTime> earliest;
    for (const auto candidate : {application, graphics, ui}) {
      if (candidate && (!earliest || *candidate < *earliest)) earliest = candidate;
    }
    return {false, earliest};
  }

} // namespace avb::desktop::detail
#endif // AVB0795_DESKTOP_UI_CADENCE_HPP
