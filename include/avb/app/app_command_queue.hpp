/// @file Main-thread command handoff between host/UI callbacks and Application.
#ifndef AVB0795_APP_APP_COMMAND_QUEUE_HPP
#define AVB0795_APP_APP_COMMAND_QUEUE_HPP

#include "avb/app/commands.hpp"

#include <deque>

namespace avb::app {

  /// FIFO of owning command values, confined to the main thread with no locking.
  /// It is separate from the mutex-protected newest-frame mailbox in core.
  class AppCommandQueue final {
  public:
    /// Append in order, returning false after close; allocation failures may propagate.
    [[nodiscard]] bool push(AppCommand command);
    /// Transfer the current batch out, leaving later pushes for a subsequent step.
    [[nodiscard]] std::deque<AppCommand> drain();
    /// Permanently reject new commands and discard commands still in the queue.
    void close() noexcept;

  private:
    std::deque<AppCommand> commands_;
    bool closed_{};
  };

}    // namespace avb::app

#endif    // AVB0795_APP_APP_COMMAND_QUEUE_HPP
