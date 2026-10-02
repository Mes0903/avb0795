/// @file
/// @brief Thread-bound owner of the browser capture visibility helper.

#ifndef AVB0795_PLATFORM_WINDOWS_FEATURES_BROWSER_CAPTURE_KEEP_ALIVE_HPP
#define AVB0795_PLATFORM_WINDOWS_FEATURES_BROWSER_CAPTURE_KEEP_ALIVE_HPP

#include "platform/windows/features/detail/browser_capture_keep_alive_policy.hpp"

#include <windows.h>

#include <chrono>
#include <expected>
#include <optional>

namespace avb::platform::windows::features {

  /// @brief Own the optional browser visibility workaround and its delayed cleanup.
  ///
  /// This feature-local owner creates an independent transparent helper window;
  /// it does not borrow a capture item, overlay, or graphics allocation. Calls
  /// and destruction belong to the creating thread. Activation is eligible only
  /// after the runtime can tick and while window capture remains connected.
  /// Verified helper visibility does not guarantee browser frame production or
  /// reliable capture of a minimized source; it proves only this helper's state.
  class BrowserCaptureKeepAlive final {
  public:
    using Clock = std::chrono::steady_clock;

    BrowserCaptureKeepAlive() noexcept;
    /// An ever-shown helper must have completed retained-owner stop before destruction.
    ~BrowserCaptureKeepAlive() noexcept;

    BrowserCaptureKeepAlive(const BrowserCaptureKeepAlive &) = delete;
    BrowserCaptureKeepAlive &operator=(const BrowserCaptureKeepAlive &) = delete;
    BrowserCaptureKeepAlive(BrowserCaptureKeepAlive &&) = delete;
    BrowserCaptureKeepAlive &operator=(BrowserCaptureKeepAlive &&) = delete;

    /// @brief Allocate and verify a hidden helper before runtime publication.
    ///
    /// An existing valid helper is accepted without changing its visibility.
    /// A colliding process-local window class is rejected rather than adopted.
    /// Never-shown preparation can attempt immediate cleanup; failed native
    /// release still retains ownership and can prevent safe destruction. Once
    /// shown, retirement requires verified hiding followed by the grace interval.
    [[nodiscard]] std::expected<void, core::AppError> prepare_hidden() noexcept;
    /// @brief Observe visibility only after validating owned identity and zero alpha.
    ///
    /// Missing windows are inactive; an invalid identity or unreadable native
    /// state is an error, not evidence that the helper is safely hidden.
    [[nodiscard]] std::expected<bool, core::AppError> snapshot_active() const noexcept;
    /// @brief Apply visibility with readback and verified restoration on failure.
    ///
    /// State-lost failure makes cleanup uncertain and blocks later activation.
    /// Hiding remains available so terminal cleanup can make further progress.
    [[nodiscard]] std::expected<void, core::AppError> set_active(bool active) noexcept;
    /// @brief Hide and verify the helper without rolling failure back to visible.
    ///
    /// Failure preserves cleanup uncertainty; it cannot authorize destruction
    /// or reset the grace interval as though hiding had been observed.
    [[nodiscard]] std::expected<void, core::AppError> deactivate() noexcept;
    [[nodiscard]] bool matches_active(bool active) const noexcept;
    /// @brief Disable further activation and initiate hiding for retained-owner stop.
    ///
    /// Continue progress_stop() calls while this owner remains alive; the request
    /// itself neither waits for the grace interval nor proves native release.
    void request_stop() noexcept;
    /// @brief Attempt eligible native retirement and report actual resource release.
    ///
    /// Window destruction precedes class unregistration. Native failures retain
    /// ownership and uncertainty for later ticks; merely reaching a deadline or
    /// retrying an operation does not count as progress.
    [[nodiscard]] bool progress_stop(Clock::time_point now) noexcept;
    [[nodiscard]] bool cleanup_pending() const noexcept;
    [[nodiscard]] bool cleanup_uncertain() const noexcept;
    /// @brief Return a future grace deadline when hidden state has been verified.
    ///
    /// An absent deadline does not prove cleanup is complete: failed hiding or
    /// native retirement still requires the runtime's independent drain polling.
    [[nodiscard]] std::optional<Clock::time_point> next_wake() const noexcept;

  private:
    struct NativeOperations;

    [[nodiscard]] bool on_owner_thread() const noexcept;
    [[nodiscard]] bool window_identity_valid() const noexcept;
    [[nodiscard]] std::expected<void, core::AppError> write_active(bool active) noexcept;
    void unwind_hidden_preparation() noexcept;

    DWORD thread_id_{};
    HINSTANCE instance_{};
    ATOM class_atom_{};
    HWND window_{};
    detail::HelperRetirementState retirement_;
    bool stopping_{};
    bool cleanup_uncertain_{};
  };

}    // namespace avb::platform::windows::features

#endif
