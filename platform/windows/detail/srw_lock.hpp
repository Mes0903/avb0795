/// @file
/// @brief Small exclusive SRW guard for short callback-state critical sections.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_SRW_LOCK_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_SRW_LOCK_HPP

#include <windows.h>

namespace avb::platform::windows::detail {

  /// @brief Serialize short callback-state transitions with lexical ownership.
  ///
  /// Acquisition can wait under contention and the lock is nonrecursive. Keep
  /// native intake, wake callbacks, and potentially reentrant WinRT calls outside
  /// the critical section. The lock owner must outlive every guard; the guard's
  /// destructor releases the lock but does not extend the protected data lifetime.
  class ExclusiveSrwLock final {
  public:
    ExclusiveSrwLock() noexcept = default;

    ExclusiveSrwLock(const ExclusiveSrwLock &) = delete;
    ExclusiveSrwLock &operator=(const ExclusiveSrwLock &) = delete;

    /// Immovable stack guard pairing each acquisition with one release.
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

    /// Return a lexical guard; the protected owner must outlive its scope.
    [[nodiscard]] Guard acquire() noexcept { return Guard{&lock_}; }

  private:
    SRWLOCK lock_ = SRWLOCK_INIT;
  };

}    // namespace avb::platform::windows::detail

#endif    // AVB0795_PLATFORM_WINDOWS_DETAIL_SRW_LOCK_HPP
