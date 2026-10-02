/// @file
/// @brief Windows same-volume replacement used by ConfigStore persistence.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_ATOMIC_REPLACE_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_ATOMIC_REPLACE_HPP

#include "avb/core/types.hpp"

#include <expected>
#include <filesystem>

namespace avb::platform::windows {

  /// @brief Commit an already written configuration temporary by same-volume rename.
  ///
  /// Success consumes the temporary path and replaces any existing destination.
  /// The persistence owner creates, writes, flushes, and cleans up the temporary;
  /// this adapter only performs replacement and reports ConfigWriteFailure with
  /// native error detail. It does not validate file contents or serialize writers.
  /// @pre Both paths refer to distinct sibling files on the same volume, and the
  /// temporary's writer has finished and closed it before replacement begins.
  [[nodiscard]] std::expected<void, core::AppError> windows_atomic_replace(
    const std::filesystem::path &temporary,
    const std::filesystem::path &destination) noexcept;

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_ATOMIC_REPLACE_HPP
