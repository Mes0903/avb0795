#ifndef AIRELINNA_CORE_APPLICATION_STATE_HPP
#define AIRELINNA_CORE_APPLICATION_STATE_HPP

#include "airelinna/core/replica_session.hpp"

#include <expected>

namespace airelinna::core {

  class ApplicationState final {
  public:
    [[nodiscard]] SessionId current_session_id() const noexcept;
    [[nodiscard]] const ReplicaSession &current_session() const noexcept;
    [[nodiscard]] std::expected<void, AppError>
    validate_session(SessionId session_id) const noexcept;

    [[nodiscard]] std::expected<SessionId, AppError>
    replace_session(ReplicaSession replacement = {});

    [[nodiscard]] std::expected<void, AppError>
    select_source(SessionId session_id, SourceDescriptor source);
    [[nodiscard]] std::expected<void, AppError>
    clear_source(SessionId session_id) noexcept;
    [[nodiscard]] std::expected<void, AppError>
    set_mode(SessionId session_id, OverlayMode mode) noexcept;
    [[nodiscard]] std::expected<void, AppError>
    set_border(SessionId session_id, BorderPreference border) noexcept;
    [[nodiscard]] std::expected<void, AppError> set_fps(SessionId session_id,
                                                        FpsCap fps_cap) noexcept;

    [[nodiscard]] std::expected<void, AppError>
    begin_start(SessionId session_id) noexcept;
    [[nodiscard]] std::expected<void, AppError>
    mark_capturing(SessionId session_id) noexcept;
    [[nodiscard]] std::expected<void, AppError>
    begin_graphics_recovery(SessionId session_id) noexcept;
    [[nodiscard]] std::expected<void, AppError>
    complete_graphics_recovery(SessionId session_id) noexcept;
    [[nodiscard]] std::expected<void, AppError>
    mark_interrupted(SessionId session_id) noexcept;
    [[nodiscard]] std::expected<void, AppError>
    begin_stop(SessionId session_id) noexcept;
    [[nodiscard]] std::expected<void, AppError>
    complete_stop(SessionId session_id) noexcept;
    [[nodiscard]] std::expected<void, AppError>
    mark_error(SessionId session_id) noexcept;

  private:
    SessionId current_session_id_{1};
    ReplicaSession current_session_;
  };

}    // namespace airelinna::core

#endif    // AIRELINNA_CORE_APPLICATION_STATE_HPP
