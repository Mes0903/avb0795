#ifndef AIRELINNA_PLATFORM_WINDOWS_DETAIL_SRW_LOCK_HPP
#define AIRELINNA_PLATFORM_WINDOWS_DETAIL_SRW_LOCK_HPP

#include <windows.h>

namespace airelinna::platform::windows::detail {

  class ExclusiveSrwLock final {
  public:
    ExclusiveSrwLock() noexcept = default;

    ExclusiveSrwLock(const ExclusiveSrwLock &) = delete;
    ExclusiveSrwLock &operator=(const ExclusiveSrwLock &) = delete;

    class Guard final {
    public:
      ~Guard() noexcept { ReleaseSRWLockExclusive(lock_); }

      Guard(const Guard &) = delete;
      Guard &operator=(const Guard &) = delete;
      Guard(Guard &&) = delete;
      Guard &operator=(Guard &&) = delete;

    private:
      friend class ExclusiveSrwLock;
      explicit Guard(PSRWLOCK lock) noexcept : lock_{lock}
      {
        AcquireSRWLockExclusive(lock_);
      }

      PSRWLOCK lock_;
    };

    [[nodiscard]] Guard acquire() noexcept { return Guard{&lock_}; }

  private:
    SRWLOCK lock_ = SRWLOCK_INIT;
  };

}    // namespace airelinna::platform::windows::detail

#endif    // AIRELINNA_PLATFORM_WINDOWS_DETAIL_SRW_LOCK_HPP
