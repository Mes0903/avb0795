/// @file
/// @brief Generation-scoped source IDs, runtime leases, and shared Closed observations.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/windows_source_registry.hpp"

#include "platform/windows/detail/item_observation_state.hpp"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace avb::platform::windows {

  namespace {

    [[nodiscard]] core::AppError source_unavailable() noexcept
    {
      return core::AppError{.code = core::AppErrorCode::SourceUnavailable};
    }

    [[nodiscard]] core::AppError refresh_unavailable() noexcept
    {
      return core::AppError{.code = core::AppErrorCode::InvalidCommand};
    }

  }    // namespace

  namespace detail {

    RuntimeWakeState::Admission::Admission(RuntimeWakeState &owner) noexcept
        : owner_{&owner} {}

    RuntimeWakeState::Admission::~Admission() noexcept
    {
      if (owner_ != nullptr) {
        owner_->leave();
      }
    }

    RuntimeWakeState::Admission::Admission(Admission &&other) noexcept
        : owner_{std::exchange(other.owner_, nullptr)} {}

    std::optional<RuntimeWakeState::Admission>
    // One CAS protects the stop/admit race; a separate Boolean could miss an entered wake.
    RuntimeWakeState::try_enter() noexcept
    {
      auto observed = admission_state_.load(std::memory_order_acquire);
      while ((observed & closed_bit) == 0) {
        if ((observed & callback_count_mask) == callback_count_mask) {
          return std::nullopt;
        }
        if (admission_state_.compare_exchange_weak(observed, observed + 1,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
          return std::optional<Admission>{Admission{*this}};
        }
      }
      return std::nullopt;
    }

    RuntimeWakeState::RuntimeWakeState(std::function<void()> wake_main_loop)
        : wake_main_loop_{std::move(wake_main_loop)}
    {
      if (!wake_main_loop_) {
        throw std::invalid_argument{"runtime wake callback is required"};
      }
    }

    void RuntimeWakeState::wake() noexcept
    {
      auto admission = try_enter();
      if (!admission) {
        return;
      }
      invoke_wake_noexcept();
    }

    // Closing admission does not revoke the registry's longer-lived Closed observation.
    void RuntimeWakeState::request_stop() noexcept
    {
      admission_state_.fetch_or(closed_bit, std::memory_order_acq_rel);
    }

    bool RuntimeWakeState::callbacks_quiescent() const noexcept
    {
      return (admission_state_.load(std::memory_order_acquire) &
              callback_count_mask) == 0;
    }

    void RuntimeWakeState::leave() noexcept
    {
      auto observed = admission_state_.load(std::memory_order_acquire);
      for (;;) {
        const auto callbacks = observed & callback_count_mask;
        if (callbacks == 0) {
          return;
        }

        if ((observed & closed_bit) != 0 && callbacks == 1) {
          const auto claimed = observed | final_wake_claimed_bit;
          if (!admission_state_.compare_exchange_weak(observed, claimed,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
            continue;
          }
          // Keep the borrowed wake target alive until its final call returns.
          // The runtime's bounded poll observes the subsequent quiescence.
          invoke_wake_noexcept();
          admission_state_.fetch_sub(1, std::memory_order_release);
          return;
        }

        if (admission_state_.compare_exchange_weak(observed, observed - 1,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
          return;
        }
      }
    }

    void RuntimeWakeState::invoke_wake_noexcept() noexcept
    {
      try {
        wake_main_loop_();
      } catch (...) {
      }
    }

    WindowsSourceIdGeneration::WindowsSourceIdGeneration() { rebuild_prefix(); }

    core::SourceId WindowsSourceIdGeneration::issue()
    {
      const auto ordinal = next_ordinal_++;
      return core::SourceId{prefix_ + std::to_string(ordinal)};
    }

    void WindowsSourceIdGeneration::advance()
    {
      ++generation_;
      next_ordinal_ = 0;
      rebuild_prefix();
    }

    std::expected<void, core::AppError>
    WindowsSourceIdGeneration::validate(const core::SourceId &id) const noexcept
    {
      if (!id.value.starts_with(prefix_)) {
        return std::unexpected(source_unavailable());
      }
      return {};
    }

    void WindowsSourceIdGeneration::rebuild_prefix()
    {
      prefix_ = "avb0795-source-" + std::to_string(generation_) + '-';
    }

    // Shared control outlives the wrapper if leases remain; its count gates refresh.
    struct WindowsSourceRegistryControl final {
      mutable std::mutex mutex;
      WindowsSourceIdGeneration id_generation;
      std::unordered_map<std::string, std::shared_ptr<detail::ItemObservationState>>
        entries;
      std::atomic_size_t active_runtime_leases{};
      bool refreshing{};
    };

  }    // namespace detail

  WindowsSourceLease::WindowsSourceLease(
    std::shared_ptr<detail::WindowsSourceRegistryControl> control,
    std::shared_ptr<detail::ItemObservationState> observation) noexcept
      : control_{std::move(control)}, observation_{std::move(observation)}
  {
    control_->active_runtime_leases.fetch_add(1, std::memory_order_acq_rel);
  }

  WindowsSourceLease::~WindowsSourceLease() { release(); }

  WindowsSourceLease::WindowsSourceLease(WindowsSourceLease &&other) noexcept
      : control_{std::move(other.control_)},
        observation_{std::move(other.observation_)} {}

  WindowsSourceLease &
  WindowsSourceLease::operator=(WindowsSourceLease &&other) noexcept
  {
    if (this != &other) {
      release();
      control_ = std::move(other.control_);
      observation_ = std::move(other.observation_);
    }
    return *this;
  }

  detail::ItemObservationState &WindowsSourceLease::observation() const noexcept
  {
    return *observation_;
  }

  void WindowsSourceLease::release() noexcept
  {
    if (!control_) {
      return;
    }
    // Drop the native item lease before advertising that refresh may retire the snapshot.
    observation_.reset();
    control_->active_runtime_leases.fetch_sub(1, std::memory_order_acq_rel);
    control_.reset();
  }

  WindowsSourceRegistry::Refresh::Refresh(WindowsSourceRegistry &owner) noexcept
      : owner_{&owner} {}

  WindowsSourceRegistry::Refresh::~Refresh() { cancel(); }

  WindowsSourceRegistry::Refresh::Refresh(Refresh &&other) noexcept
      : owner_{std::exchange(other.owner_, nullptr)} {}

  WindowsSourceRegistry::Refresh &
  WindowsSourceRegistry::Refresh::operator=(Refresh &&other) noexcept
  {
    if (this != &other) {
      cancel();
      owner_ = std::exchange(other.owner_, nullptr);
    }
    return *this;
  }

  std::expected<std::vector<core::SourceId>, core::AppError>
  WindowsSourceRegistry::Refresh::commit(
    std::vector<std::shared_ptr<detail::ItemObservationState>> observations)
  {
    if (owner_ == nullptr) {
      return std::unexpected(refresh_unavailable());
    }

    auto *const owner = owner_;
    std::expected<std::vector<core::SourceId>, core::AppError> result;
    try {
      result = owner->commit_refresh(std::move(observations));
    } catch (...) {
      owner->cancel_refresh();
      owner_ = nullptr;
      throw;
    }
    owner_ = nullptr;
    if (!result) {
      owner->cancel_refresh();
    }
    return result;
  }

  void WindowsSourceRegistry::Refresh::cancel() noexcept
  {
    if (owner_ != nullptr) {
      owner_->cancel_refresh();
      owner_ = nullptr;
    }
  }

  WindowsSourceRegistry::WindowsSourceRegistry()
      : control_{std::make_shared<detail::WindowsSourceRegistryControl>()} {}

  WindowsSourceRegistry::~WindowsSourceRegistry()
  {
    std::unordered_map<std::string, std::shared_ptr<detail::ItemObservationState>>
      retired_entries;
    {
      const std::scoped_lock lock{control_->mutex};
      control_->refreshing = true;
      retired_entries = std::move(control_->entries);
    }
    for (const auto &[id, observation] : retired_entries) {
      static_cast<void>(id);
      observation->retire();
    }
  }

  std::expected<WindowsSourceRegistry::Refresh, core::AppError>
  WindowsSourceRegistry::begin_refresh()
  {
    const std::scoped_lock lock{control_->mutex};
    if (control_->refreshing ||
        control_->active_runtime_leases.load(std::memory_order_acquire) != 0) {
      return std::unexpected(refresh_unavailable());
    }

    control_->refreshing = true;
    return Refresh{*this};
  }

  std::expected<WindowsSourceLease, core::AppError>
  WindowsSourceRegistry::resolve(const core::SourceId &id) const
  {
    const std::scoped_lock lock{control_->mutex};
    if (control_->refreshing || !control_->id_generation.validate(id)) {
      return std::unexpected(source_unavailable());
    }

    const auto entry = control_->entries.find(id.value);
    if (entry == control_->entries.end()) {
      return std::unexpected(source_unavailable());
    }
    return WindowsSourceLease{control_, entry->second};
  }

  std::expected<std::vector<core::SourceId>, core::AppError>
  WindowsSourceRegistry::commit_refresh(
    std::vector<std::shared_ptr<detail::ItemObservationState>> observations)
  {
    detail::WindowsSourceIdGeneration next_generation;
    {
      const std::scoped_lock lock{control_->mutex};
      if (!control_->refreshing) {
        return std::unexpected(refresh_unavailable());
      }
      next_generation = control_->id_generation;
    }
    // Prepare allocating replacement containers before mutating the published snapshot.
    next_generation.advance();

    std::vector<core::SourceId> ids;
    ids.reserve(observations.size());
    std::unordered_map<std::string, std::shared_ptr<detail::ItemObservationState>>
      next_entries;
    next_entries.reserve(observations.size());
    for (auto &observation : observations) {
      auto id = next_generation.issue();
      next_entries.emplace(id.value, std::move(observation));
      ids.push_back(std::move(id));
    }

    std::vector<std::shared_ptr<detail::ItemObservationState>> old_observations;
    {
      const std::scoped_lock lock{control_->mutex};
      old_observations.reserve(control_->entries.size());
      for (const auto &[id, observation] : control_->entries) {
        static_cast<void>(id);
        old_observations.push_back(observation);
      }
    }
    // Native revocation stays outside the registry mutex to avoid reentrant deadlock.
    for (const auto &observation : old_observations) {
      observation->retire();
    }

    {
      const std::scoped_lock lock{control_->mutex};
      control_->entries = std::move(next_entries);
      control_->id_generation = std::move(next_generation);
      control_->refreshing = false;
    }
    return ids;
  }

  void WindowsSourceRegistry::cancel_refresh() noexcept
  {
    const std::scoped_lock lock{control_->mutex};
    control_->refreshing = false;
  }

  namespace detail {

    ItemObservationState::ItemObservationState(
      ConstructionKey,
      winrt::Windows::Graphics::Capture::GraphicsCaptureItem item) noexcept
        : item_{std::move(item)} {}

    ItemObservationState::~ItemObservationState() { retire(); }

    std::shared_ptr<ItemObservationState> ItemObservationState::create(
      winrt::Windows::Graphics::Capture::GraphicsCaptureItem item)
    {
      auto state = std::make_shared<ItemObservationState>(ConstructionKey{},
                                                          std::move(item));
      // A delegate may outlive registry refresh but can reach only this typed state.
      const std::weak_ptr<ItemObservationState> weak_state{state};
      state->closed_token_ =
        state->item_.Closed([weak_state](const auto &, const auto &) noexcept {
          if (const auto locked = weak_state.lock()) {
            locked->record_closed();
          }
        });
      state->handler_registered_ = true;
      return state;
    }

    const winrt::Windows::Graphics::Capture::GraphicsCaptureItem &
    ItemObservationState::item() const noexcept
    {
      return item_;
    }

    bool ItemObservationState::is_closed() const noexcept
    {
      return closed_.load(std::memory_order_acquire);
    }

    std::uint64_t ItemObservationState::closed_sequence() const noexcept
    {
      return closed_sequence_.load(std::memory_order_acquire);
    }

    void ItemObservationState::bind_runtime_wake(
      const std::shared_ptr<RuntimeWakeState> &wake) noexcept
    {
      const auto wake_guard = runtime_wake_lock_.acquire();
      runtime_wake_ = wake;
    }

    void ItemObservationState::unbind_runtime_wake(
      const RuntimeWakeState *const wake) noexcept
    {
      const auto wake_guard = runtime_wake_lock_.acquire();
      const auto current = runtime_wake_.lock();
      if (current.get() == wake) {
        runtime_wake_.reset();
      }
    }

    void ItemObservationState::retire() noexcept
    {
      callback_admission_.store(false, std::memory_order_release);
      if (!handler_registered_) {
        return;
      }

      try {
        item_.Closed(closed_token_);
      } catch (...) {
      }
      handler_registered_ = false;
    }

    void ItemObservationState::record_closed() noexcept
    {
      if (!callback_admission_.load(std::memory_order_acquire)) {
        return;
      }
      callbacks_in_flight_.fetch_add(1, std::memory_order_acq_rel);
      if (!callback_admission_.load(std::memory_order_acquire)) {
        callbacks_in_flight_.fetch_sub(1, std::memory_order_acq_rel);
        return;
      }

      bool expected = false;
      if (closed_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        closed_sequence_.store(1, std::memory_order_release);
        // Snapshot the weak binding under its short lock; invoke foreign wake code unlocked.
        std::shared_ptr<RuntimeWakeState> wake;
        {
          const auto wake_guard = runtime_wake_lock_.acquire();
          wake = runtime_wake_.lock();
        }
        if (wake) {
          wake->wake();
        }
      }
      callbacks_in_flight_.fetch_sub(1, std::memory_order_acq_rel);
    }

  }    // namespace detail

}    // namespace avb::platform::windows
