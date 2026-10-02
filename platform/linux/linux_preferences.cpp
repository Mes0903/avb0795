/// @file
/// @brief Linux directory setup and retained descriptor-based writer coordination.
#include "platform/linux/linux_preferences.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <utility>

namespace avb::linux_platform {
  namespace {
    core::AppError failure(int detail)
    {
      return {core::AppErrorCode::ConfigWriteFailure, detail};
    }
    struct Descriptor final {
      int value{-1};
      ~Descriptor()
      {
        if (value >= 0) close(value);
      }
    };
  }    // namespace

  LinuxPreferences::LinuxPreferences(std::filesystem::path path, int lock) noexcept
      : path_{std::move(path)}, lock_{lock} {}
  LinuxPreferences::~LinuxPreferences()
  {
    if (lock_ >= 0) close(lock_);
  }
  LinuxPreferences::LinuxPreferences(LinuxPreferences &&other) noexcept
      : path_{std::move(other.path_)}, lock_{std::exchange(other.lock_, -1)} {}
  LinuxPreferences &LinuxPreferences::operator=(LinuxPreferences &&other) noexcept
  {
    if (this != &other) {
      if (lock_ >= 0) close(lock_);
      path_ = std::move(other.path_);
      lock_ = std::exchange(other.lock_, -1);
    }
    return *this;
  }

  std::expected<LinuxPreferences, core::AppError> LinuxPreferences::create()
  {
    try {
      std::filesystem::path root;
      if (const auto *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        const std::filesystem::path candidate{xdg};
        if (candidate.is_absolute()) root = candidate;
      }
      if (root.empty()) {
        const auto *home = std::getenv("HOME");
        if (!home || !*home) return std::unexpected{failure(EINVAL)};
        root = std::filesystem::path{home} / ".config";
      }
      const auto directory = root / "avb0795";
      auto path = directory / "avb0795.config";
      std::filesystem::create_directories(directory);
      // Operate only on our directory; reject a final symlink rather than chmod
      // someone else's directory through it. XDG parents keep their own modes.
      Descriptor directory_fd{open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
      if (directory_fd.value < 0) return std::unexpected{failure(errno)};
      if (fchmod(directory_fd.value, 0700) != 0) return std::unexpected{failure(errno)};
      Descriptor lock{openat(directory_fd.value, "avb0795.config.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)};
      if (lock.value < 0) return std::unexpected{failure(errno)};
      struct stat info{};
      if (fstat(lock.value, &info) != 0) return std::unexpected{failure(errno)};
      if (!S_ISREG(info.st_mode)) return std::unexpected{failure(EINVAL)};
      // Lock a persistent inode, not the config that each save replaces. flock's
      // independent open descriptions also reject a second owner in this process.
      if (flock(lock.value, LOCK_EX | LOCK_NB) != 0) return std::unexpected{failure(errno)};
      if (fchmod(lock.value, 0600) != 0) return std::unexpected{failure(errno)};
      return LinuxPreferences{std::move(path), std::exchange(lock.value, -1)};
    } catch (const std::filesystem::filesystem_error &error) {
      return std::unexpected{failure(error.code().value())};
    } catch (...) {
      return std::unexpected{failure(ENOMEM)};
    }
  }
}    // namespace avb::linux_platform
