#include "platform/windows/windows_capture_source.hpp"

#include "platform/windows/detail/item_observation_state.hpp"

#include <new>
#include <utility>

namespace airelinna::platform::windows {

class WindowsCaptureSource::Impl final {
public:
  Impl(WindowsSourceLease lease, detail::ItemObservationState &observation,
       WakeMainLoop wake_main_loop)
      : lease_{std::move(lease)}, observation_{&observation},
        wake_state_{std::make_shared<detail::RuntimeWakeState>(
            std::move(wake_main_loop))} {
    observation_->bind_runtime_wake(wake_state_);
  }

  ~Impl() { request_stop(); }

  [[nodiscard]] bool consume_closed_edge() noexcept {
    const auto observed = observation_->closed_sequence();
    if (observed == closed_cursor_) {
      return false;
    }
    closed_cursor_ = observed;
    return true;
  }

  [[nodiscard]] bool is_closed() const noexcept {
    return observation_->is_closed();
  }

  void request_stop() noexcept {
    if (stop_requested_) {
      return;
    }
    stop_requested_ = true;
    wake_state_->request_stop();
    observation_->unbind_runtime_wake(wake_state_.get());
  }

  [[nodiscard]] bool callbacks_quiescent() const noexcept {
    return wake_state_->callbacks_quiescent();
  }

  [[nodiscard]] const winrt::Windows::Graphics::Capture::GraphicsCaptureItem &
  item() const noexcept {
    return observation_->item();
  }

private:
  WindowsSourceLease lease_;
  detail::ItemObservationState *observation_{};
  std::shared_ptr<detail::RuntimeWakeState> wake_state_;
  std::uint64_t closed_cursor_{};
  bool stop_requested_{};
};

std::expected<std::unique_ptr<WindowsCaptureSource>, core::AppError>
WindowsCaptureSource::create(WindowsSourceLease lease,
                             WakeMainLoop wake_main_loop) {
  if (!wake_main_loop) {
    return std::unexpected{
        core::AppError{.code = core::AppErrorCode::RuntimeConstructionFailure}};
  }
  try {
    auto *const observation = &lease.observation();
    auto implementation = std::make_unique<Impl>(std::move(lease), *observation,
                                                 std::move(wake_main_loop));
    return std::unique_ptr<WindowsCaptureSource>{
        new WindowsCaptureSource{std::move(implementation)}};
  } catch (const winrt::hresult_error &error) {
    return std::unexpected{
        core::AppError{.code = core::AppErrorCode::RuntimeConstructionFailure,
                       .native_detail = error.code().value}};
  } catch (const std::bad_alloc &) {
    return std::unexpected{
        core::AppError{.code = core::AppErrorCode::RuntimeConstructionFailure,
                       .native_detail = E_OUTOFMEMORY}};
  } catch (...) {
    return std::unexpected{
        core::AppError{.code = core::AppErrorCode::RuntimeConstructionFailure,
                       .native_detail = E_FAIL}};
  }
}

WindowsCaptureSource::WindowsCaptureSource(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)} {}

WindowsCaptureSource::~WindowsCaptureSource() = default;

bool WindowsCaptureSource::consume_closed_edge() noexcept {
  return implementation_->consume_closed_edge();
}

bool WindowsCaptureSource::is_closed() const noexcept {
  return implementation_->is_closed();
}

void WindowsCaptureSource::request_stop() noexcept {
  implementation_->request_stop();
}

bool WindowsCaptureSource::callbacks_quiescent() const noexcept {
  return implementation_->callbacks_quiescent();
}

const winrt::Windows::Graphics::Capture::GraphicsCaptureItem &
WindowsCaptureSource::item() const noexcept {
  return implementation_->item();
}

} // namespace airelinna::platform::windows
