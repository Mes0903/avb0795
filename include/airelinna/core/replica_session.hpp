/// @file Pure session values and guarded visible lifecycle transitions.
#ifndef AIRELINNA_CORE_REPLICA_SESSION_HPP
#define AIRELINNA_CORE_REPLICA_SESSION_HPP

#include "airelinna/core/types.hpp"

#include <expected>
#include <optional>

namespace airelinna::core {

  /// Stores selection/settings and enforces lifecycle edges without native side effects.
  /// Settings setters do not enforce application command availability; Application
  /// applies runtime transactions before committing these values.
  class ReplicaSession final {
  public:
    /// Borrow the owned catalog snapshot, if a source has been selected.
    [[nodiscard]] const std::optional<SourceDescriptor> &
    selected_source() const noexcept;
    /// Read visible state, which need not match the runtime's resource drain phase.
    [[nodiscard]] SessionLifecycle lifecycle() const noexcept;
    /// Borrow the current accepted settings; no platform objects are referenced.
    [[nodiscard]] const ReplicaSettings &settings() const noexcept;

    /// Replace the source value only; selection never starts native capture.
    void select_source(SourceDescriptor source);
    /// Remove only the selected descriptor.
    void clear_source() noexcept;
    /// Store the accepted session-only input mode.
    void set_mode(OverlayMode mode) noexcept;
    /// Store the accepted border preference; persistence belongs to Application.
    void set_border(BorderPreference border) noexcept;
    /// Store the accepted render cap; scheduler lifetime belongs to the runtime.
    void set_fps(FpsCap fps_cap) noexcept;

    /// Require a selected source and Idle/Error, otherwise return InvalidCommand.
    [[nodiscard]] std::expected<void, AppError> begin_start() noexcept;
    /// Confirm a successful start; valid only from Starting.
    [[nodiscard]] std::expected<void, AppError> mark_capturing() noexcept;
    /// Enter RecoveringGraphics from Capturing, or self-transition while Interrupted.
    [[nodiscard]] std::expected<void, AppError>
    begin_graphics_recovery() noexcept;
    /// Return recovery to Capturing while preserving Interrupted after source loss.
    [[nodiscard]] std::expected<void, AppError>
    complete_graphics_recovery() noexcept;
    /// Record source disconnection from Capturing, RecoveringGraphics or Interrupted.
    [[nodiscard]] std::expected<void, AppError> mark_interrupted() noexcept;
    /// Begin visible stopping from Capturing, RecoveringGraphics or Interrupted.
    [[nodiscard]] std::expected<void, AppError> begin_stop() noexcept;
    /// Accept completed ordinary stopping; retain selection and settings for reuse.
    [[nodiscard]] std::expected<void, AppError> complete_stop() noexcept;
    /// Enter Error from starting/active/interrupted/error states; reject Idle/Stopping.
    [[nodiscard]] std::expected<void, AppError> mark_error() noexcept;

  private:
    /// Reject invalid edges without mutating the previous lifecycle.
    [[nodiscard]] std::expected<void, AppError>
    transition_to(SessionLifecycle target, bool allowed) noexcept;

    std::optional<SourceDescriptor> selected_source_;
    SessionLifecycle lifecycle_{SessionLifecycle::Idle};
    ReplicaSettings settings_;
  };

}    // namespace airelinna::core

#endif    // AIRELINNA_CORE_REPLICA_SESSION_HPP
