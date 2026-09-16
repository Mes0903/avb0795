/// @file
/// @brief Generation-checked access to the single V1 domain session.
#ifndef AVB0795_CORE_APPLICATION_STATE_HPP
#define AVB0795_CORE_APPLICATION_STATE_HPP

#include "avb/core/replica_session.hpp"

#include <expected>

namespace avb::core {

  /// @brief Own one session value and its generation, with no runtime resources or I/O.
  /// Application sequences mutations on the main thread; this class does not lock.
  /// Every SessionId-taking mutation rejects stale IDs before touching the session.
  /// Generation validation establishes identity only: lifecycle edges are checked
  /// by the session, while capability checks and native transactions belong to
  /// the caller. Rejected identity or lifecycle checks leave domain values unchanged.
  class ApplicationState final {
  public:
    /// Return the token the controller must attach to session-specific commands.
    [[nodiscard]] SessionId current_session_id() const noexcept;
    /// Borrow the current value, not a snapshot. Later mutations or replacement
    /// change what the reference observes; do not retain nested views across them.
    [[nodiscard]] const ReplicaSession &current_session() const noexcept;
    /// Return InvalidCommand when the supplied generation is no longer current.
    [[nodiscard]] std::expected<void, AppError>
    validate_session(SessionId session_id) const noexcept;

    /// @brief Consume a replacement value and issue a new generation token.
    /// The prior token becomes stale even when replacement has identical settings.
    /// Generation exhaustion returns InvalidValue without replacing the session.
    /// This operation does not stop a runtime; Application must arrange ownership.
    [[nodiscard]] std::expected<SessionId, AppError>
    replace_session(ReplicaSession replacement = {});

    /// @brief Move an owned source descriptor into the current generation.
    /// Selection resets its source crop and interaction, without starting capture; the caller
    /// supplies an accepted descriptor and enforces selection availability.
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
    /// Commit an accepted opacity percentage after validating its generation.
    [[nodiscard]] std::expected<void, AppError>
    set_opacity(SessionId session_id, int percent) noexcept;
    /// Commit an accepted source-pixel crop after validating its generation.
    [[nodiscard]] std::expected<void, AppError>
    set_region(SessionId session_id, std::optional<PixelRegion> region) noexcept;
    /// Commit transient selection intent without changing persisted preferences.
    [[nodiscard]] std::expected<void, AppError>
    set_region_selection(SessionId session_id, bool active) noexcept;
    /// Commit accepted experimental preferences without invoking a backend.
    [[nodiscard]] std::expected<void, AppError>
    set_extensions(SessionId session_id, ExtensionSettings extensions) noexcept;

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

}    // namespace avb::core

#endif    // AVB0795_CORE_APPLICATION_STATE_HPP
