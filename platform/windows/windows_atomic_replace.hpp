/// @file
/// @brief Windows same-volume replacement used by ConfigStore persistence.
/// Native implementation details remain below the portable application ports.

#ifndef AIRELINNA_PLATFORM_WINDOWS_WINDOWS_ATOMIC_REPLACE_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WINDOWS_ATOMIC_REPLACE_HPP

#include "airelinna/core/types.hpp"

#include <expected>
#include <filesystem>

namespace airelinna::platform::windows {

  /// Replace ConfigStore's sibling destination after it writes/flushes the temp file.
  /// Same-volume MoveFileExW failure is returned with fixed native error detail.
  [[nodiscard]] std::expected<void, core::AppError> windows_atomic_replace(
    const std::filesystem::path &temporary,
    const std::filesystem::path &destination) noexcept;

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_ATOMIC_REPLACE_HPP
