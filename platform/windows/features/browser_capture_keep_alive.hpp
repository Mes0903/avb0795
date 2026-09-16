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

    [[nodiscard]] std::expected<void, core::AppError> prepare_hidden() noexcept;
    [[nodiscard]] std::expected<bool, core::AppError> snapshot_active() const noexcept;
    [[nodiscard]] std::expected<void, core::AppError> set_active(bool active) noexcept;
    [[nodiscard]] std::expected<void, core::AppError> deactivate() noexcept;
    [[nodiscard]] bool matches_active(bool active) const noexcept;
    void request_stop() noexcept;
    /// True only when this call released at least one owned native resource.
    [[nodiscard]] bool progress_stop(Clock::time_point now) noexcept;
    [[nodiscard]] bool cleanup_pending() const noexcept;
    [[nodiscard]] bool cleanup_uncertain() const noexcept;
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
