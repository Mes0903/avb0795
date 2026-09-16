/// @file
/// @brief Shared item identity and weak, counted runtime wake lifetime.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_ITEM_OBSERVATION_STATE_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_ITEM_OBSERVATION_STATE_HPP

#include "platform/windows/detail/srw_lock.hpp"

#include <unknwn.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/base.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace avb::platform::windows::detail {

  /// Counts borrowed host wake calls through their complete execution.
  /// A shared atomic word linearizes admission against terminal source stop.
  class RuntimeWakeState final {
  private:
    /// Scope guard whose final wake remains counted until the callable returns.
    class Admission final {
    public:
      ~Admission() noexcept;

      Admission(const Admission &) = delete;
      Admission &operator=(const Admission &) = delete;
      Admission(Admission &&other) noexcept;
      Admission &operator=(Admission &&) = delete;

    private:
      friend class RuntimeWakeState;
      explicit Admission(RuntimeWakeState &owner) noexcept;

      RuntimeWakeState *owner_{};
    };

  public:
    explicit RuntimeWakeState(std::function<void()> wake_main_loop);

    RuntimeWakeState(const RuntimeWakeState &) = delete;
    RuntimeWakeState &operator=(const RuntimeWakeState &) = delete;

    /// Admit and invoke the immutable wake callable, containing any exception.
    void wake() noexcept;
    /// Reject future calls atomically; entered calls finish independently.
    void request_stop() noexcept;
    /// Used by runtime drain before destroying the wake target's owning host.
    [[nodiscard]] bool callbacks_quiescent() const noexcept;

  private:
    [[nodiscard]] std::optional<Admission> try_enter() noexcept;
    void leave() noexcept;
    void invoke_wake_noexcept() noexcept;

    // Admission and count share one CAS domain, so stop cannot miss a new caller.
    static constexpr std::uint64_t closed_bit = std::uint64_t{1} << 63;
    static constexpr std::uint64_t final_wake_claimed_bit = std::uint64_t{1}
                                                            << 62;
    static constexpr std::uint64_t callback_count_mask =
      final_wake_claimed_bit - 1;

    std::function<void()> wake_main_loop_;
    std::atomic_uint64_t admission_state_{};
  };

  /// Canonical GraphicsCaptureItem identity and sole pre-publication Closed token.
  /// Delegates weak-lock this self-contained state; they never borrow a registry.
  class ItemObservationState final {
  private:
    struct ConstructionKey {};

  public:
    ItemObservationState(
      ConstructionKey,
      winrt::Windows::Graphics::Capture::GraphicsCaptureItem item) noexcept;
    ~ItemObservationState();

    ItemObservationState(const ItemObservationState &) = delete;
    ItemObservationState &operator=(const ItemObservationState &) = delete;
    ItemObservationState(ItemObservationState &&) = delete;
    ItemObservationState &operator=(ItemObservationState &&) = delete;

    /// Install the Closed watcher before exposing the observation to a descriptor.
    [[nodiscard]] static std::shared_ptr<ItemObservationState>
    create(winrt::Windows::Graphics::Capture::GraphicsCaptureItem item);

    /// Borrow the original COM item while a registry or runtime lease retains it.
    [[nodiscard]] const winrt::Windows::Graphics::Capture::GraphicsCaptureItem &
    item() const noexcept;
    /// Sticky closure remains readable even without an active runtime wake.
    [[nodiscard]] bool is_closed() const noexcept;
    /// Monotonic close marker consumed independently by a source lease's cursor.
    [[nodiscard]] std::uint64_t closed_sequence() const noexcept;
    /// Attach a weak wake binding only after runtime publication is guaranteed.
    void
    bind_runtime_wake(const std::shared_ptr<RuntimeWakeState> &wake) noexcept;
    /// Clear only the matching binding; a replacement binding must be preserved.
    void unbind_runtime_wake(const RuntimeWakeState *wake) noexcept;
    /// Reject late delegates and revoke the sole watcher without joining callbacks.
    void retire() noexcept;

  private:
    void record_closed() noexcept;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::event_token closed_token_{};
    bool handler_registered_{};
    std::atomic_bool callback_admission_{true};
    std::atomic_size_t callbacks_in_flight_{};
    std::atomic_bool closed_{};
    std::atomic_uint64_t closed_sequence_{};
    // This lock protects weak binding access, never invocation of foreign code.
    mutable ExclusiveSrwLock runtime_wake_lock_;
    std::weak_ptr<RuntimeWakeState> runtime_wake_;
  };

}    // namespace avb::platform::windows::detail

#endif    // AVB0795_PLATFORM_WINDOWS_DETAIL_ITEM_OBSERVATION_STATE_HPP
