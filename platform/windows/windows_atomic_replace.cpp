/// @file
/// @brief Windows same-volume replacement used by ConfigStore persistence.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/windows_atomic_replace.hpp"

#include <windows.h>

#include <cstdint>
#include <expected>

namespace avb::platform::windows {
  namespace {

    [[nodiscard]] constexpr std::expected<void, core::AppError>
    write_failure(const DWORD native_detail) noexcept
    {
      return std::unexpected{core::AppError{
        core::AppErrorCode::ConfigWriteFailure,
        static_cast<std::int64_t>(native_detail)}};
    }

  }    // namespace

  std::expected<void, core::AppError> windows_atomic_replace(
    const std::filesystem::path &temporary,
    const std::filesystem::path &destination) noexcept
  {
    // ConfigStore always supplies sibling paths. A same-volume rename avoids
    // ReplaceFileW's documented partial-failure states while preserving the
    // old destination whenever the rename itself is rejected.
    constexpr DWORD move_flags{MOVEFILE_REPLACE_EXISTING |
                               MOVEFILE_WRITE_THROUGH};
    // ConfigStore owns temp creation, flush, and cleanup; this adapter only performs replace.
    if (MoveFileExW(temporary.c_str(), destination.c_str(), move_flags) ==
        FALSE) {
      return write_failure(GetLastError());
    }
    return {};
  }

}    // namespace avb::platform::windows
