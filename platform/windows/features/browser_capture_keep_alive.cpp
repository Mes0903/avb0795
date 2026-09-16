/// @file
/// @brief Isolated native helper visibility and checked, delayed retirement.

#include "platform/windows/features/browser_capture_keep_alive.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

namespace avb::platform::windows::features {

  namespace {
    constexpr wchar_t helper_class_name[] = L"TaskListThumbnailWnd";
    constexpr DWORD helper_extended_style =
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT;

    [[nodiscard]] core::AppError native_failure() noexcept
    {
      return {.code = core::AppErrorCode::PlatformOperationFailure,
              .native_detail = static_cast<std::int64_t>(GetLastError())};
    }

    [[nodiscard]] constexpr core::AppError state_lost() noexcept
    {
      return {.code = core::AppErrorCode::RuntimeStateLost};
    }

    [[nodiscard]] std::optional<LONG_PTR> window_value(HWND window, int index) noexcept
    {
      SetLastError(ERROR_SUCCESS);
      const auto value = GetWindowLongPtrW(window, index);
      if (value == 0 && GetLastError() != ERROR_SUCCESS) {
        return std::nullopt;
      }
      return value;
    }
  }    // namespace

  struct BrowserCaptureKeepAlive::NativeOperations final {
    BrowserCaptureKeepAlive &owner;

    [[nodiscard]] std::expected<bool, core::AppError> observe_active() const noexcept
    {
      return owner.snapshot_active();
    }

    [[nodiscard]] std::expected<void, core::AppError> prepare_hidden() noexcept
    {
      return owner.prepare_hidden();
    }

    [[nodiscard]] std::expected<void, core::AppError> write_active(bool active) noexcept
    {
      return owner.write_active(active);
    }

    [[nodiscard]] bool matches_active(bool active) noexcept
    {
      const bool matches = owner.matches_active(active);
      if (!matches) {
        owner.retirement_.hidden_verified = false;
      }
      return matches;
    }

    [[nodiscard]] unsigned resource_count() const noexcept
    {
      return static_cast<unsigned>(owner.window_ != nullptr) +
             static_cast<unsigned>(owner.class_atom_ != 0);
    }

    [[nodiscard]] bool destroy_window() noexcept
    {
      if (!owner.window_) {
        return true;
      }
      if (!owner.on_owner_thread() || !owner.window_identity_valid()) {
        owner.retirement_.hidden_verified = false;
        return false;
      }
      if (IsWindowVisible(owner.window_)) {
        owner.retirement_.ever_shown = true;
        owner.retirement_.hidden_verified = false;
        return false;
      }
      if (!DestroyWindow(owner.window_)) {
        return false;
      }
      owner.window_ = nullptr;
      return true;
    }

    [[nodiscard]] bool unregister_class() noexcept
    {
      if (!owner.class_atom_) {
        return true;
      }
      if (owner.window_ || !owner.on_owner_thread() ||
          !UnregisterClassW(helper_class_name, owner.instance_)) {
        return false;
      }
      owner.class_atom_ = 0;
      return true;
    }
  };

  BrowserCaptureKeepAlive::BrowserCaptureKeepAlive() noexcept
      : thread_id_{GetCurrentThreadId()}, instance_{GetModuleHandleW(nullptr)}
  {
  }

  BrowserCaptureKeepAlive::~BrowserCaptureKeepAlive() noexcept
  {
    if (cleanup_pending() && !retirement_.ever_shown) {
      unwind_hidden_preparation();
    }
    if (cleanup_pending()) {
      // Published ownership must remain in the runtime drain until fully retired.
      std::terminate();
    }
  }

  bool BrowserCaptureKeepAlive::on_owner_thread() const noexcept
  {
    return GetCurrentThreadId() == thread_id_;
  }

  bool BrowserCaptureKeepAlive::window_identity_valid() const noexcept
  {
    if (!on_owner_thread() || !instance_ || !window_ || !class_atom_) {
      return false;
    }
    DWORD process{};
    if (GetWindowThreadProcessId(window_, &process) != thread_id_ ||
        process != GetCurrentProcessId()) {
      return false;
    }
    std::array<wchar_t, 64> name{};
    const auto length = GetClassNameW(window_, name.data(), static_cast<int>(name.size()));
    if (length <= 0 || std::wstring_view{name.data(), static_cast<std::size_t>(length)} != helper_class_name ||
        GetClassLongPtrW(window_, GCW_ATOM) != class_atom_) {
      return false;
    }
    const auto instance = window_value(window_, GWLP_HINSTANCE);
    const auto parent = window_value(window_, GWLP_HWNDPARENT);
    const auto style = window_value(window_, GWL_STYLE);
    const auto extended = window_value(window_, GWL_EXSTYLE);
    return instance && *instance == reinterpret_cast<LONG_PTR>(instance_) &&
           parent && *parent == 0 && style &&
           (static_cast<DWORD>(*style) & (WS_POPUP | WS_CHILD)) == WS_POPUP &&
           extended && static_cast<DWORD>(*extended) == helper_extended_style;
  }

  std::expected<bool, core::AppError>
  BrowserCaptureKeepAlive::snapshot_active() const noexcept
  {
    if (!on_owner_thread()) {
      return std::unexpected{state_lost()};
    }
    if (!window_) {
      return false;
    }
    if (!window_identity_valid()) {
      return std::unexpected{state_lost()};
    }
    COLORREF color{};
    BYTE alpha{};
    DWORD flags{};
    if (!GetLayeredWindowAttributes(window_, &color, &alpha, &flags)) {
      auto error = native_failure();
      error.code = core::AppErrorCode::RuntimeStateLost;
      return std::unexpected{error};
    }
    if (alpha != 0 || flags != LWA_ALPHA) {
      return std::unexpected{state_lost()};
    }
    return IsWindowVisible(window_) != FALSE;
  }

  bool BrowserCaptureKeepAlive::matches_active(bool active) const noexcept
  {
    const auto observed = snapshot_active();
    return observed && *observed == active;
  }

  std::expected<void, core::AppError>
  BrowserCaptureKeepAlive::prepare_hidden() noexcept
  {
    if (!on_owner_thread()) {
      return std::unexpected{state_lost()};
    }
    if (stopping_ || cleanup_uncertain_) {
      return std::unexpected{core::AppError{.code = core::AppErrorCode::InvalidCommand}};
    }
    if (window_) {
      const auto observed = snapshot_active();
      if (!observed) {
        cleanup_uncertain_ = true;
        retirement_.hidden_verified = false;
        return std::unexpected{observed.error()};
      }
      return {};
    }
    if (!instance_) {
      return std::unexpected{state_lost()};
    }
    if (!class_atom_) {
      WNDCLASSEXW window_class{};
      window_class.cbSize = sizeof(window_class);
      window_class.lpfnWndProc = DefWindowProcW;
      window_class.hInstance = instance_;
      window_class.lpszClassName = helper_class_name;
      class_atom_ = RegisterClassExW(&window_class);
      if (!class_atom_) {
        // A colliding process-local class is never adopted.
        return std::unexpected{native_failure()};
      }
    }
    window_ = CreateWindowExW(helper_extended_style, helper_class_name, L"",
                              WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance_, nullptr);
    if (!window_) {
      const auto error = native_failure();
      unwind_hidden_preparation();
      return std::unexpected{error};
    }
    if (!SetLayeredWindowAttributes(window_, 0, 0, LWA_ALPHA)) {
      const auto error = native_failure();
      unwind_hidden_preparation();
      return std::unexpected{error};
    }
    if (!matches_active(false)) {
      const auto error = native_failure();
      unwind_hidden_preparation();
      return std::unexpected{error};
    }
    return {};
  }

  void BrowserCaptureKeepAlive::unwind_hidden_preparation() noexcept
  {
    if (!on_owner_thread() || retirement_.ever_shown) {
      cleanup_uncertain_ = true;
      return;
    }
    if (window_) {
      retirement_.hidden_verified = window_identity_valid() && !IsWindowVisible(window_);
    }
    NativeOperations operations{*this};
    const auto result = detail::progress_helper_retirement(operations, retirement_, Clock::now());
    cleanup_uncertain_ = cleanup_uncertain_ || result.ownership_uncertain || cleanup_pending();
    if (!cleanup_pending()) {
      cleanup_uncertain_ = false;
    }
  }

  std::expected<void, core::AppError>
  BrowserCaptureKeepAlive::write_active(bool active) noexcept
  {
    if (!on_owner_thread()) {
      return std::unexpected{state_lost()};
    }
    if (active && (stopping_ || cleanup_uncertain_)) {
      return std::unexpected{core::AppError{.code = core::AppErrorCode::InvalidCommand}};
    }
    if (!window_) {
      if (!active) {
        return {};
      }
      return std::unexpected{state_lost()};
    }
    if (!window_identity_valid()) {
      retirement_.hidden_verified = false;
      return std::unexpected{state_lost()};
    }
    const bool visible = IsWindowVisible(window_) != FALSE;
    retirement_.ever_shown = retirement_.ever_shown || visible;
    if (visible) {
      retirement_.hidden_verified = false;
    }
    if (visible == active && (active || retirement_.hidden_verified)) {
      return {};
    }
    // A delivered SHOW with failed readback must still receive a full HIDE grace.
    retirement_.ever_shown = retirement_.ever_shown || active;
    retirement_.hidden_verified = false;
    const auto flags = SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                       SWP_NOOWNERZORDER | (active ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    if (!SetWindowPos(window_, nullptr, 0, 0, 0, 0, flags)) {
      return std::unexpected{native_failure()};
    }
    if (!matches_active(active)) {
      return std::unexpected{state_lost()};
    }
    if (!active) {
      // Sample after native HIDE and identity/attributes/visibility readback.
      retirement_.last_hide = Clock::now();
      retirement_.hidden_verified = true;
    }
    return {};
  }

  std::expected<void, core::AppError>
  BrowserCaptureKeepAlive::set_active(bool active) noexcept
  {
    if (stopping_ || cleanup_uncertain_) {
      if (active) {
        return std::unexpected{core::AppError{.code = core::AppErrorCode::InvalidCommand}};
      }
      return deactivate();
    }
    NativeOperations operations{*this};
    const auto result = detail::set_helper_active(operations, active);
    if (!result && result.error().code == core::AppErrorCode::RuntimeStateLost) {
      cleanup_uncertain_ = true;
      retirement_.hidden_verified = false;
    }
    return result;
  }

  std::expected<void, core::AppError> BrowserCaptureKeepAlive::deactivate() noexcept
  {
    NativeOperations operations{*this};
    const auto result = detail::deactivate_helper(operations);
    if (!result) {
      cleanup_uncertain_ = true;
      retirement_.hidden_verified = false;
    }
    return result;
  }

  void BrowserCaptureKeepAlive::request_stop() noexcept
  {
    stopping_ = true;
    (void) deactivate();
  }

  bool BrowserCaptureKeepAlive::progress_stop(Clock::time_point now) noexcept
  {
    if (!stopping_) {
      return false;
    }
    if (!on_owner_thread()) {
      cleanup_uncertain_ = true;
      return false;
    }
    if (!retirement_.hidden_verified) {
      (void) deactivate();
    }
    // A failed never-shown configuration needs only owned identity and hidden proof.
    if (!retirement_.ever_shown && window_) {
      retirement_.hidden_verified = window_identity_valid() && !IsWindowVisible(window_);
    }
    NativeOperations operations{*this};
    const auto result = detail::progress_helper_retirement(operations, retirement_, now);
    cleanup_uncertain_ = cleanup_uncertain_ || result.ownership_uncertain;
    if (!cleanup_pending()) {
      cleanup_uncertain_ = false;
    }
    return result.made_progress;
  }

  bool BrowserCaptureKeepAlive::cleanup_pending() const noexcept
  {
    return window_ != nullptr || class_atom_ != 0;
  }

  bool BrowserCaptureKeepAlive::cleanup_uncertain() const noexcept
  {
    return cleanup_uncertain_;
  }

  std::optional<BrowserCaptureKeepAlive::Clock::time_point>
  BrowserCaptureKeepAlive::next_wake() const noexcept
  {
    if (!stopping_ || !cleanup_pending() || !retirement_.hidden_verified) {
      return std::nullopt;
    }
    const auto deadline = detail::helper_retirement_deadline(retirement_);
    return deadline && *deadline > Clock::now() ? deadline : std::nullopt;
  }

}    // namespace avb::platform::windows::features
