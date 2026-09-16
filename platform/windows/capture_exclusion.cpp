/// @file
/// @brief Best-effort capture exclusion and validation of the patched GLFW window class.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/capture_exclusion.hpp"

#include "platform/windows/detail/overlay_policy.hpp"

#include <cstdint>
#include <optional>

namespace avb::platform::windows {

  namespace {

    using SetWindowDisplayAffinityFn = BOOL(WINAPI *)(HWND, DWORD);

    struct WindowIdentity final {
      DWORD owner_thread{};
      DWORD owner_process{};

      friend bool operator==(const WindowIdentity &,
                             const WindowIdentity &) = default;
    };

    [[nodiscard]] std::optional<WindowIdentity>
    owned_top_level_window_identity(HWND window) noexcept
    {
      if (window == nullptr || IsWindow(window) == FALSE) {
        return std::nullopt;
      }

      DWORD process_id{};
      const DWORD thread_id = GetWindowThreadProcessId(window, &process_id);
      if (thread_id == 0 || thread_id != GetCurrentThreadId() ||
          process_id != GetCurrentProcessId() ||
          GetAncestor(window, GA_ROOT) != window) {
        return std::nullopt;
      }

      return WindowIdentity{.owner_thread = thread_id, .owner_process = process_id};
    }

    [[nodiscard]] bool is_unsupported_error(const DWORD error) noexcept
    {
      switch (error) {
      case ERROR_INVALID_FUNCTION:
      case ERROR_NOT_SUPPORTED:
      case ERROR_CALL_NOT_IMPLEMENTED:
      case ERROR_PROC_NOT_FOUND:
        return true;
      default:
        return false;
      }
    }

  }    // namespace

  core::CaptureExclusionStatus attempt_capture_exclusion(HWND window) noexcept
  {
    using core::CaptureExclusionStatus;

    // Affinity applies only to a live top-level HWND owned by this process/thread.
    const auto identity = owned_top_level_window_identity(window);
    if (!identity) {
      return CaptureExclusionStatus::Failed;
    }

    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
      return CaptureExclusionStatus::Failed;
    }

    const auto set_affinity = reinterpret_cast<SetWindowDisplayAffinityFn>(
      GetProcAddress(user32, "SetWindowDisplayAffinity"));
    if (set_affinity == nullptr) {
      return CaptureExclusionStatus::Unsupported;
    }

    SetLastError(ERROR_SUCCESS);
    const bool set_succeeded =
      set_affinity(window, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    const DWORD error = set_succeeded ? ERROR_SUCCESS : GetLastError();

    // HWND values can be reused; revalidate owner identity after the native operation.
    const auto current_identity = owned_top_level_window_identity(window);
    if (!current_identity || *current_identity != *identity) {
      return CaptureExclusionStatus::Failed;
    }
    if (set_succeeded) {
      return CaptureExclusionStatus::Excluded;
    }
    if (is_unsupported_error(error)) {
      return CaptureExclusionStatus::Unsupported;
    }
    return CaptureExclusionStatus::Failed;
  }

  std::expected<core::CaptureExclusionStatus, core::AppError>
  prepare_controller_window(HWND window) noexcept
  {
    if (window == nullptr || IsWindow(window) == FALSE) {
      return std::unexpected{core::AppError{
        core::AppErrorCode::RuntimeConstructionFailure,
        static_cast<std::int64_t>(ERROR_INVALID_WINDOW_HANDLE)}};
    }

    SetLastError(ERROR_SUCCESS);
    const auto class_style = GetClassLongPtrW(window, GCL_STYLE);
    const auto query_error = GetLastError();
    if (class_style == 0 && query_error != ERROR_SUCCESS) {
      return std::unexpected{core::AppError{
        core::AppErrorCode::RuntimeConstructionFailure,
        static_cast<std::int64_t>(query_error)}};
    }

    // Reject incompatible class styles before advertising a usable overlay input policy.
    if (!detail::is_overlay_class_style_supported(
          static_cast<std::uintptr_t>(class_style))) {
      return std::unexpected{core::AppError{
        core::AppErrorCode::UnsupportedWindowClass,
        static_cast<std::int64_t>(class_style)}};
    }

    // Product windows participate in screenshots and screen sharing. Keep the
    // native class validation above while preserving the new HWND's default
    // capture policy; this status also keeps exclusion warnings inactive.
    return core::CaptureExclusionStatus::NotAttempted;
  }

}    // namespace avb::platform::windows
