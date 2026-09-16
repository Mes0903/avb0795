/// @file
/// @brief Main-thread FIFO transfer; commands retain ordering and owning value payloads.
#include "avb/app/app_command_queue.hpp"

#include <utility>

namespace avb::app {

  bool AppCommandQueue::push(AppCommand command)
  {
    if (closed_) {
      return false;
    }

    commands_.push_back(std::move(command));
    return true;
  }

  std::deque<AppCommand> AppCommandQueue::drain()
  {
    std::deque<AppCommand> drained;
    // Isolate this step's batch. Reentrant/later producers target the next batch.
    drained.swap(commands_);
    return drained;
  }

  void AppCommandQueue::close() noexcept
  {
    closed_ = true;
    // Application separately stops iterating an already-drained batch after Quit.
    commands_.clear();
  }

}    // namespace avb::app
