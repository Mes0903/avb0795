/// @file
/// @brief One-slot owning handoff with replacement instead of a frame backlog.
#ifndef AVB0795_CORE_LATEST_VALUE_MAILBOX_HPP
#define AVB0795_CORE_LATEST_VALUE_MAILBOX_HPP

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace avb::core {

  /// @brief Transfer the newest pending value between synchronized producers and consumers.
  ///
  /// A pending value is replaceable work, so consumers receive freshness rather
  /// than a FIFO history. Taking a value transfers its ownership out of the slot;
  /// later submissions cannot replace a value already held by a consumer.
  /// Displaced payload owners are released after unlocking, allowing their final
  /// release to reenter the mailbox. Moves and moved-from destruction still run
  /// under the mutex and must not reenter it.
  ///
  /// @pre Keep the mailbox alive until every concurrent operation has finished;
  /// close() stops admission but does not synchronize destruction with callers.
  /// @note Throwing payload moves can change the slot and counters before failure;
  /// this generic container does not provide a strong exception guarantee.
  template <typename T>
  class LatestValueMailbox final {
  public:
    /// @brief Identify the admission edge observed while holding the mutex.
    /// BecameReady lets a producer wake an idle consumer; replacing already-pending
    /// work needs no extra wake. A consumer or reentrant release can empty the slot
    /// before submit() returns, so this is not a current-readiness query.
    enum class SubmitResult { BecameReady,
                              ReplacedPending,
                              Closed };

    /// @brief Count submissions admitted before payload insertion, replacements
    /// transferred out of the slot, and successful takes; closed rejects do not count.
    struct Counters {
      std::uint64_t submitted{};
      std::uint64_t replaced{};
      std::uint64_t taken{};
    };

    /// @brief Consume the supplied owner, replacing older pending work if open.
    /// Closed rejection still consumes the by-value argument and releases it on
    /// return. Payload move failures propagate; admission counters are not rolled back.
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

    /// @brief Transfer the newest pending owner to the caller, leaving the slot empty.
    /// An empty result means no value was pending at this operation's lock boundary;
    /// it does not distinguish an open empty mailbox from a closed one.
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

    /// @brief Permanently reject future submissions and release any pending payload.
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

    /// @brief Copy one coherent counter snapshot while holding the mailbox mutex.
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
