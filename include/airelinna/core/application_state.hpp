/// @file Generation-checked access to the single V1 domain session.
#ifndef AIRELINNA_CORE_APPLICATION_STATE_HPP
#define AIRELINNA_CORE_APPLICATION_STATE_HPP

#include "airelinna/core/replica_session.hpp"

#include <expected>

namespace airelinna::core {

  /// Owns one session value and its generation, with no runtime resources or I/O.
  /// Application sequences mutations on the main thread; this class does not lock.
  /// Every SessionId-taking mutation rejects stale IDs before touching the session.
  class ApplicationState final {
  public:
    /// Return the token the controller must attach to session-specific commands.
    [[nodiscard]] SessionId current_session_id() const noexcept;
    /// Borrow the current value; callers must not retain it across owner destruction.
    [[nodiscard]] const ReplicaSession &current_session() const noexcept;
    /// Return InvalidCommand when the supplied generation is no longer current.
    [[nodiscard]] std::expected<void, AppError>
    validate_session(SessionId session_id) const noexcept;

    /// Replace the value and increment its token; reject generation overflow.
    /// This operation does not stop a runtime; Application must arrange ownership.
    [[nodiscard]] std::expected<SessionId, AppError>
    replace_session(ReplicaSession replacement = {});

    /// Store a source descriptor after generation validation; does not start capture.
    [[nodiscard]] std::expected<void, AppError>
    select_source(SessionId session_id, SourceDescriptor source);
    /// Clear selection after generation validation; lifecycle policy belongs to Application.
    [[nodiscard]] std::expected<void, AppError>
    clear_source(SessionId session_id) noexcept;
    /// Commit an accepted input-mode value without invoking a backend.
    [[nodiscard]] std::expected<void, AppError>
    set_mode(SessionId session_id, OverlayMode mode) noexcept;
    /// Commit an accepted border value without performing persistence.
    [[nodiscard]] std::expected<void, AppError>
    set_border(SessionId session_id, BorderPreference border) noexcept;
    /// Commit an accepted render-cap value without configuring a scheduler directly.
    [[nodiscard]] std::expected<void, AppError> set_fps(SessionId session_id,
                                                        FpsCap fps_cap) noexcept;

    /// Validate generation and enter Starting from a selected Idle/Error session.
    [[nodiscard]] std::expected<void, AppError>
    begin_start(SessionId session_id) noexcept;
    /// Confirm construction by entering Capturing from Starting.
    [[nodiscard]] std::expected<void, AppError>
    mark_capturing(SessionId session_id) noexcept;
    /// Mark graphics recovery, preserving an already Interrupted lifecycle.
    [[nodiscard]] std::expected<void, AppError>
    begin_graphics_recovery(SessionId session_id) noexcept;
    /// Complete graphics recovery without implying that a lost source returned.
    [[nodiscard]] std::expected<void, AppError>
    complete_graphics_recovery(SessionId session_id) noexcept;
    /// Record source loss while leaving native runtime ownership to Application.
    [[nodiscard]] std::expected<void, AppError>
    mark_interrupted(SessionId session_id) noexcept;
    /// Enter Stopping; this alone is not proof that native teardown completed.
    [[nodiscard]] std::expected<void, AppError>
    begin_stop(SessionId session_id) noexcept;
    /// Enter Idle from Stopping after Application receives safe runtime completion.
    [[nodiscard]] std::expected<void, AppError>
    complete_stop(SessionId session_id) noexcept;
    /// Enter or preserve Error without discarding selected source/settings.
    [[nodiscard]] std::expected<void, AppError>
    mark_error(SessionId session_id) noexcept;

  private:
    SessionId current_session_id_{1};
    ReplicaSession current_session_;
  };

}    // namespace airelinna::core

#endif    // AIRELINNA_CORE_APPLICATION_STATE_HPP
