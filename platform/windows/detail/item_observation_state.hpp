#ifndef AIRELINNA_PLATFORM_WINDOWS_DETAIL_ITEM_OBSERVATION_STATE_HPP
#define AIRELINNA_PLATFORM_WINDOWS_DETAIL_ITEM_OBSERVATION_STATE_HPP

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

namespace airelinna::platform::windows::detail {

class RuntimeWakeState final {
private:
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

  void wake() noexcept;
  void request_stop() noexcept;
  [[nodiscard]] bool callbacks_quiescent() const noexcept;

private:
  [[nodiscard]] std::optional<Admission> try_enter() noexcept;
  void leave() noexcept;
  void invoke_wake_noexcept() noexcept;

  static constexpr std::uint64_t closed_bit = std::uint64_t{1} << 63;
  static constexpr std::uint64_t final_wake_claimed_bit = std::uint64_t{1}
                                                          << 62;
  static constexpr std::uint64_t callback_count_mask =
      final_wake_claimed_bit - 1;

  std::function<void()> wake_main_loop_;
  std::atomic_uint64_t admission_state_{};
};

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

  [[nodiscard]] static std::shared_ptr<ItemObservationState>
  create(winrt::Windows::Graphics::Capture::GraphicsCaptureItem item);

  [[nodiscard]] const winrt::Windows::Graphics::Capture::GraphicsCaptureItem &
  item() const noexcept;
  [[nodiscard]] bool is_closed() const noexcept;
  [[nodiscard]] std::uint64_t closed_sequence() const noexcept;
  void
  bind_runtime_wake(const std::shared_ptr<RuntimeWakeState> &wake) noexcept;
  void unbind_runtime_wake(const RuntimeWakeState *wake) noexcept;
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
  mutable ExclusiveSrwLock runtime_wake_lock_;
  std::weak_ptr<RuntimeWakeState> runtime_wake_;
};

} // namespace airelinna::platform::windows::detail

#endif // AIRELINNA_PLATFORM_WINDOWS_DETAIL_ITEM_OBSERVATION_STATE_HPP
