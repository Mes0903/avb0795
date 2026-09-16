/// @file
/// @brief Nonblocking serialization of native frame pull through mailbox publication.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_FRAME_INTAKE_GATE_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_FRAME_INTAKE_GATE_HPP

#include <atomic>
#include <cstdint>

namespace avb::platform::windows::detail {

  /// Serializes native acquisition, validation, and mailbox publication as one unit.
  /// A losing callback only records contention; it does not wait for the claimant.
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

    // Called only by the claimant, after native pull and mailbox publication.
    // Marking contention in the same atomic word prevents a lost release wake.
    /// Release intake and report whether an overlapping caller needs rearming.
    /// The caller still holds callback admission while rearming and waking.
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
