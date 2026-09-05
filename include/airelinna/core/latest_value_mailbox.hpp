#ifndef AIRELINNA_CORE_LATEST_VALUE_MAILBOX_HPP
#define AIRELINNA_CORE_LATEST_VALUE_MAILBOX_HPP

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace airelinna::core {

  template <typename T>
  class LatestValueMailbox final {
  public:
    enum class SubmitResult { BecameReady,
                              ReplacedPending,
                              Closed };

    struct Counters {
      std::uint64_t submitted{};
      std::uint64_t replaced{};
      std::uint64_t taken{};
    };

    [[nodiscard]] SubmitResult submit(T value)
    {
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

}    // namespace airelinna::core

#endif    // AIRELINNA_CORE_LATEST_VALUE_MAILBOX_HPP
