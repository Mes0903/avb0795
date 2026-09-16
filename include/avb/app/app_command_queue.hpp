/// @file
/// @brief Main-thread command handoff between host/UI callbacks and Application.
#ifndef AVB0795_APP_APP_COMMAND_QUEUE_HPP
#define AVB0795_APP_APP_COMMAND_QUEUE_HPP

#include "avb/app/commands.hpp"

#include <deque>

namespace avb::app {

  /// @brief Preserve every accepted user intention in FIFO order on the main thread.
  /// Command order carries meaning, such as Start followed by Stop or settings
  /// before Quit, so values are never coalesced as they are in the frame mailbox.
  /// Owning payloads allow delivery after the producing widget call returns.
  /// There is no lock or wake mechanism; producers and the consumer share one
  /// thread, and the host supplies any scheduling needed to process queued work.
  class AppCommandQueue final {
  public:
    /// @brief Consume and append a command, or return false after permanent closure.
    /// Success means queued, not accepted by application policy or executed.
    /// Allocation failures may propagate; a rejected by-value payload is discarded.
    [[nodiscard]] bool push(AppCommand command);
    /// @brief Transfer the whole current batch into an independently owned deque.
    /// Later pushes remain for the next drain, even if produced while the caller
    /// handles this batch. Closing the queue cannot revoke an already-drained batch.
    [[nodiscard]] std::deque<AppCommand> drain();
    /// @brief Permanently reject new commands and discard commands still in the queue.
    void close() noexcept;

  private:
    std::deque<AppCommand> commands_;
    bool closed_{};
  };

}    // namespace avb::app

#endif    // AVB0795_APP_APP_COMMAND_QUEUE_HPP
