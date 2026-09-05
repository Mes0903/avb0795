#include "airelinna/core/application_state.hpp"

#include <limits>
#include <utility>

namespace airelinna::core {

  SessionId ApplicationState::current_session_id() const noexcept
  {
    return current_session_id_;
  }

  const ReplicaSession &ApplicationState::current_session() const noexcept
  {
    return current_session_;
  }

  std::expected<void, AppError>
  ApplicationState::validate_session(SessionId session_id) const noexcept
  {
    if (session_id != current_session_id_) {
      return std::unexpected(AppError{AppErrorCode::InvalidCommand});
    }

    return {};
  }

  std::expected<SessionId, AppError>
  ApplicationState::replace_session(ReplicaSession replacement)
  {
    if (current_session_id_.value == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(AppError{AppErrorCode::InvalidValue});
    }

    current_session_ = std::move(replacement);
    ++current_session_id_.value;
    return current_session_id_;
  }

  std::expected<void, AppError>
  ApplicationState::select_source(SessionId session_id, SourceDescriptor source)
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    current_session_.select_source(std::move(source));
    return {};
  }

  std::expected<void, AppError>
  ApplicationState::clear_source(SessionId session_id) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    current_session_.clear_source();
    return {};
  }

  std::expected<void, AppError>
  ApplicationState::set_mode(SessionId session_id, OverlayMode mode) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    current_session_.set_mode(mode);
    return {};
  }

  std::expected<void, AppError>
  ApplicationState::set_border(SessionId session_id,
                               BorderPreference border) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    current_session_.set_border(border);
    return {};
  }

  std::expected<void, AppError>
  ApplicationState::set_fps(SessionId session_id, FpsCap fps_cap) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    current_session_.set_fps(fps_cap);
    return {};
  }

  std::expected<void, AppError>
  ApplicationState::begin_start(SessionId session_id) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    return current_session_.begin_start();
  }

  std::expected<void, AppError>
  ApplicationState::mark_capturing(SessionId session_id) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    return current_session_.mark_capturing();
  }

  std::expected<void, AppError>
  ApplicationState::begin_graphics_recovery(SessionId session_id) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    return current_session_.begin_graphics_recovery();
  }

  std::expected<void, AppError>
  ApplicationState::complete_graphics_recovery(SessionId session_id) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    return current_session_.complete_graphics_recovery();
  }

  std::expected<void, AppError>
  ApplicationState::mark_interrupted(SessionId session_id) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    return current_session_.mark_interrupted();
  }

  std::expected<void, AppError>
  ApplicationState::begin_stop(SessionId session_id) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    return current_session_.begin_stop();
  }

  std::expected<void, AppError>
  ApplicationState::complete_stop(SessionId session_id) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    return current_session_.complete_stop();
  }

  std::expected<void, AppError>
  ApplicationState::mark_error(SessionId session_id) noexcept
  {
    if (auto result = validate_session(session_id); !result) {
      return result;
    }

    return current_session_.mark_error();
  }

}    // namespace airelinna::core
