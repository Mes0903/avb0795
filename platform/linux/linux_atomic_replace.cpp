/// @file
/// @brief Pre-commit Linux temporary validation, permission fixing and synchronization.
#include "platform/linux/linux_atomic_replace.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

namespace avb::linux_platform {
  std::expected<void, core::AppError> linux_atomic_replace(
    const std::filesystem::path &temporary, const std::filesystem::path &destination)
  {
    const auto failure = [](int detail) {
      return std::unexpected{core::AppError{core::AppErrorCode::ConfigWriteFailure, detail}};
    };
    try {
      const auto source = temporary.lexically_normal();
      const auto target = destination.lexically_normal();
      if (source == target || source.parent_path() != target.parent_path()) return failure(EINVAL);
      const int descriptor = open(temporary.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
      if (descriptor < 0) return failure(errno);
      struct Guard final {
        int descriptor;
        ~Guard()
        {
          if (descriptor >= 0) close(descriptor);
        }
      } guard{descriptor};
      struct stat info{};
      if (fstat(descriptor, &info) != 0) return failure(errno);
      if (!S_ISREG(info.st_mode)) return failure(EINVAL);
      if (fchmod(descriptor, 0600) != 0) return failure(errno);
      int synced;
      do {
        synced = fsync(descriptor);
      } while (synced != 0 && errno == EINTR);
      if (synced != 0) return failure(errno);
      // Even close failure must precede commit. Do not retry close on Linux:
      // an interrupted close already releases the descriptor for reuse.
      guard.descriptor = -1;
      if (close(descriptor) != 0) return failure(errno);
      if (rename(temporary.c_str(), destination.c_str()) != 0) return failure(errno);
      // No fallible work after commit: a reported failure must retain the old file.
      return {};
    } catch (...) {
      return failure(ENOMEM);
    }
  }
}    // namespace avb::linux_platform
