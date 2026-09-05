#include "platform/windows/capture_exclusion.hpp"

#include <optional>

namespace airelinna::platform::windows {

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

}    // namespace airelinna::platform::windows
