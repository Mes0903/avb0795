/// @file
/// @brief Nonblocking serialization of native frame pull through mailbox publication.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_FRAME_INTAKE_GATE_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_FRAME_INTAKE_GATE_HPP

#include <atomic>
#include <cstdint>

namespace avb::platform::windows::detail {

  /// @brief Coalesce overlapping intake attempts without a waiting consumer queue.
  ///
  /// The claimant owns native acquisition through mailbox publication. A loser
  /// sets a single pending bit, so many arrivals request one later service pass
  /// rather than accumulating stale frames or blocking a native callback thread.
  /// Admission and generation checks remain the caller's responsibility.
  class FrameIntakeGate final {
  public:
    /// Claim exclusive intake, or atomically request one later service opportunity.
    [[nodiscard]] bool try_claim() noexcept
    {
      auto observed = state_.load(std::memory_order_acquire);
      for (;;) {
        const auto available = (observed & busy) == 0;
        const auto desired = available ? busy : observed | pending;
        if (state_.compare_exchange_weak(observed, desired,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
          return available;
        }
        // Retry a changed atomic state, never wait for the current intake.
      }
    }

    /// @brief Release intake and report contention that needs another service pass.
    ///
    /// Pending and ownership share one atomic word to avoid losing contention at
    /// release. The caller retains callback admission while rearming and waking,
    /// which keeps borrowed host access covered by the callback lifetime proof.
    /// @pre Only the current claimant calls this, after acquisition and publication.
    [[nodiscard]] bool release() noexcept
    {
      return (state_.exchange(0, std::memory_order_acq_rel) & pending) != 0;
    }

  private:
    static constexpr std::uint32_t busy = 1;
    static constexpr std::uint32_t pending = 2;
    std::atomic_uint32_t state_{};
  };

}    // namespace avb::platform::windows::detail

#endif    // AVB0795_PLATFORM_WINDOWS_DETAIL_FRAME_INTAKE_GATE_HPP
