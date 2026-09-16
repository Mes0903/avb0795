/// @file
/// @brief Native full-client sizing and hit testing for the controller window.
#include "platform/windows/windows_controller_window.hpp"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <exception>

namespace avb::platform::windows {
  namespace {

    // The procedure plus this ID identifies one attachment per native window.
    constexpr UINT_PTR controller_subclass_id{1};
    constexpr int logical_dpi{96};
    constexpr int logical_resize_band{8};
    constexpr int logical_minimum_width{320};
    constexpr int logical_minimum_height{240};
    constexpr UINT frame_change_flags{SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                       SWP_NOACTIVATE | SWP_FRAMECHANGED};

    [[nodiscard]] core::AppError construction_error(DWORD native_detail) noexcept
    {
      return {.code = core::AppErrorCode::RuntimeConstructionFailure,
              .native_detail = static_cast<std::int64_t>(native_detail)};
    }

    [[nodiscard]] int window_dpi(HWND window) noexcept
    {
      const auto dpi = GetDpiForWindow(window);
      return dpi == 0 ? logical_dpi : static_cast<int>(dpi);
    }

    [[nodiscard]] bool write_style(HWND window, LONG_PTR style) noexcept
    {
      // Zero can be a valid previous style, so inspect last-error only then.
      SetLastError(ERROR_SUCCESS);
      const auto previous = SetWindowLongPtrW(window, GWL_STYLE, style);
      return previous != 0 || GetLastError() == ERROR_SUCCESS;
    }

    [[nodiscard]] core::AppError restore_after_failure(
      HWND window, LONG_PTR original_style, core::AppError failure) noexcept
    {
      const auto restored = write_style(window, original_style);
      const auto reframed = SetWindowPos(window, nullptr, 0, 0, 0, 0, frame_change_flags);
      if (!restored || !reframed || !IsWindow(window) ||
          GetWindowLongPtrW(window, GWL_STYLE) != original_style) {
        return {.code = core::AppErrorCode::RuntimeStateLost,
                .native_detail = failure.native_detail};
      }
      return failure;
    }

  }    // namespace

  WindowsControllerWindow::WindowsControllerWindow(DWORD owner_thread_id) noexcept
    : owner_thread_id_{owner_thread_id}
  {
  }

  std::expected<std::unique_ptr<WindowsControllerWindow>, core::AppError>
  WindowsControllerWindow::create(HWND window)
  {
    if (!IsWindow(window)) {
      return std::unexpected{construction_error(ERROR_INVALID_WINDOW_HANDLE)};
    }
    DWORD process_id{};
    const auto window_thread = GetWindowThreadProcessId(window, &process_id);
    if (process_id != GetCurrentProcessId()) {
      return std::unexpected{construction_error(ERROR_ACCESS_DENIED)};
    }
    if (window_thread != GetCurrentThreadId()) {
      return std::unexpected{construction_error(ERROR_INVALID_THREAD_ID)};
    }
    DWORD_PTR existing_receiver{};
    if (GetWindowSubclass(window, subclass_proc, controller_subclass_id,
                          &existing_receiver)) {
      return std::unexpected{construction_error(ERROR_ALREADY_EXISTS)};
    }
    SetLastError(ERROR_SUCCESS);
    const auto original_style = GetWindowLongPtrW(window, GWL_STYLE);
    if (original_style == 0 && GetLastError() != ERROR_SUCCESS) {
      return std::unexpected{construction_error(GetLastError())};
    }

    // Private construction keeps callback storage stable; allocate before any
    // native mutation so allocation failure needs no rollback.
    auto owner = std::unique_ptr<WindowsControllerWindow>{
      new WindowsControllerWindow{window_thread}};
    const auto controller_style =
      (original_style & ~static_cast<LONG_PTR>(WS_CAPTION | WS_MAXIMIZEBOX)) |
      WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX;
    if (!write_style(window, controller_style)) {
      return std::unexpected{construction_error(GetLastError())};
    }
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowSubclass(window, subclass_proc, controller_subclass_id,
                           reinterpret_cast<DWORD_PTR>(owner.get()))) {
      return std::unexpected{restore_after_failure(
        window, original_style, construction_error(GetLastError()))};
    }
    owner->window_ = window;
    // Frame recalculation must already reach our full-client callback.
    if (!SetWindowPos(window, nullptr, 0, 0, 0, 0, frame_change_flags)) {
      const auto failure = construction_error(GetLastError());
      owner->detach();
      return std::unexpected{restore_after_failure(window, original_style, failure)};
    }
    return owner;
  }

  WindowsControllerWindow::~WindowsControllerWindow()
  {
    detach();
  }

  void WindowsControllerWindow::detach() noexcept
  {
    if (!window_) return;
    if (GetCurrentThreadId() != owner_thread_id_) std::terminate();
    if (!RemoveWindowSubclass(window_, subclass_proc, controller_subclass_id)) {
      DWORD_PTR receiver{};
      if (GetWindowSubclass(window_, subclass_proc, controller_subclass_id,
                            &receiver)) {
        std::terminate();
      }
    }
    window_ = nullptr;
  }

  void WindowsControllerWindow::set_drag_region(float right_fraction,
                                                float bottom_fraction) noexcept
  {
    if (GetCurrentThreadId() != owner_thread_id_) return;
    if (!std::isfinite(right_fraction) || !std::isfinite(bottom_fraction)) {
      drag_right_ = 0.0F;
      drag_bottom_ = 0.0F;
      return;
    }
    drag_right_ = std::clamp(right_fraction, 0.0F, 1.0F);
    drag_bottom_ = std::clamp(bottom_fraction, 0.0F, 1.0F);
  }

  LRESULT WindowsControllerWindow::hit_test(HWND window, LPARAM coordinates) const noexcept
  {
    POINT cursor{GET_X_LPARAM(coordinates), GET_Y_LPARAM(coordinates)};
    RECT outer{};
    if (!GetWindowRect(window, &outer) || cursor.x < outer.left ||
        cursor.x >= outer.right || cursor.y < outer.top || cursor.y >= outer.bottom) {
      return HTCLIENT;
    }
    const auto band = (std::max)(1, MulDiv(logical_resize_band, window_dpi(window), logical_dpi));
    const auto horizontal_band = (std::min)(static_cast<LONG>(band), (outer.right - outer.left) / 2);
    const auto vertical_band = (std::min)(static_cast<LONG>(band), (outer.bottom - outer.top) / 2);
    const auto left = cursor.x < outer.left + horizontal_band;
    const auto right = cursor.x >= outer.right - horizontal_band;
    const auto top = cursor.y < outer.top + vertical_band;
    const auto bottom = cursor.y >= outer.bottom - vertical_band;
    if (top) return left ? HTTOPLEFT : (right ? HTTOPRIGHT : HTTOP);
    if (bottom) return left ? HTBOTTOMLEFT : (right ? HTBOTTOMRIGHT : HTBOTTOM);
    if (left) return HTLEFT;
    if (right) return HTRIGHT;

    RECT client{};
    if (!ScreenToClient(window, &cursor) || !GetClientRect(window, &client)) {
      return HTCLIENT;
    }
    if (cursor.x >= 0 && cursor.y >= 0 &&
        static_cast<double>(cursor.x) < static_cast<double>(client.right) * drag_right_ &&
        static_cast<double>(cursor.y) < static_cast<double>(client.bottom) * drag_bottom_) {
      return HTCAPTION;
    }
    return HTCLIENT;
  }

  LRESULT CALLBACK WindowsControllerWindow::subclass_proc(
    HWND window, UINT message, WPARAM word_parameter, LPARAM long_parameter,
    UINT_PTR, DWORD_PTR receiver) noexcept
  {
    auto &owner = *reinterpret_cast<WindowsControllerWindow *>(receiver);
    switch (message) {
    case WM_NCCALCSIZE:
      // Both message forms already contain the proposed outer rectangle.
      return 0;
    case WM_NCHITTEST:
      return owner.hit_test(window, long_parameter);
    case WM_GETMINMAXINFO: {
      const auto result = DefSubclassProc(window, message, word_parameter, long_parameter);
      if (long_parameter != 0) {
        auto &limits = *reinterpret_cast<MINMAXINFO *>(long_parameter);
        const auto dpi = window_dpi(window);
        limits.ptMinTrackSize = {MulDiv(logical_minimum_width, dpi, logical_dpi),
                                MulDiv(logical_minimum_height, dpi, logical_dpi)};
      }
      return result;
    }
    case WM_NCLBUTTONDBLCLK:
      if (word_parameter == HTCAPTION) return 0;
      break;
    case WM_NCDESTROY:
      // Clear before forwarding: the next procedure may release the owner.
      owner.detach();
      break;
    default:
      break;
    }
    // Size, DPI, close and keyboard messages retain their upstream semantics.
    return DefSubclassProc(window, message, word_parameter, long_parameter);
  }

}    // namespace avb::platform::windows
