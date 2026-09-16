/// @file One-slot owning handoff with replacement instead of a frame backlog.
#ifndef AVB0795_CORE_LATEST_VALUE_MAILBOX_HPP
#define AVB0795_CORE_LATEST_VALUE_MAILBOX_HPP

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace avb::core {

  /// Mutex-protected newest-value handoff; supports move-only owning payloads.
  /// The mailbox owns at most one pending value. Destruction of displaced payload
  /// owners happens after unlocking; moved-from values are reset under the lock.
  /// Callers must keep this object alive until all concurrent operations finish.
  template <typename T>
  class LatestValueMailbox final {
  public:
    /// BecameReady identifies the empty-to-ready edge a producer may use to wake
    /// its consumer. Replacement need not repeatedly wake a capped scheduler.
    enum class SubmitResult { BecameReady,
                              ReplacedPending,
                              Closed };

    /// Accepted submissions, overwrites, and successful takes; closed rejects are excluded.
    struct Counters {
      std::uint64_t submitted{};
      std::uint64_t replaced{};
      std::uint64_t taken{};
    };

    /// Take ownership by value, replacing older work or discarding it when closed.
    /// Payload move operations may throw; no generic no-throw guarantee is imposed.
    [[nodiscard]] SubmitResult submit(T value)
    {
      // Keep the displaced owner outside the critical section for its final release.
      std::optional<T> replaced;
      SubmitResult result{SubmitResult::Closed};

      {
        const std::lock_guard lock{mutex_};
        if (!closed_) {
          ++counters_.submitted;
          if (pending_) {
            replaced.emplace(std::move(*pending_));
            pending_.reset();
            ++counters_.replaced;
            result = SubmitResult::ReplacedPending;
          }
          else {
            result = SubmitResult::BecameReady;
          }
          pending_.emplace(std::move(value));
        }
      }

      return result;
    }

    /// Transfer the newest pending value to the consumer, leaving the slot empty.
    [[nodiscard]] std::optional<T> take()
    {
      std::optional<T> value;

      {
        const std::lock_guard lock{mutex_};
        if (pending_) {
          value.emplace(std::move(*pending_));
          pending_.reset();
          ++counters_.taken;
        }
      }

      return value;
    }

    /// Permanently reject future submissions and release any pending payload.
    /// This does not join producers or prove that external callbacks have quiesced.
    void close()
    {
      std::optional<T> pending;

      {
        const std::lock_guard lock{mutex_};
        closed_ = true;
        if (pending_) {
          pending.emplace(std::move(*pending_));
          pending_.reset();
        }
      }
    }

    /// Copy one coherent counter snapshot while holding the mailbox mutex.
    [[nodiscard]] Counters counters() const
    {
      const std::lock_guard lock{mutex_};
      return counters_;
    }

  private:
    mutable std::mutex mutex_;
    std::optional<T> pending_;
    Counters counters_;
    bool closed_{};
  };

}    // namespace avb::core

#endif    // AVB0795_CORE_LATEST_VALUE_MAILBOX_HPP
