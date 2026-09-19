/// @file
/// @brief Pure session values and guarded visible lifecycle transitions.
#ifndef AVB0795_CORE_REPLICA_SESSION_HPP
#define AVB0795_CORE_REPLICA_SESSION_HPP

#include "avb/core/types.hpp"

#include <expected>
#include <optional>

namespace avb::core {

  /// @brief Store selection/settings and enforce lifecycle edges without native side effects.
  /// Settings setters do not enforce application command availability; Application
  /// applies runtime transactions before committing these values.
  /// All rejected lifecycle edges return InvalidCommand without changing lifecycle,
  /// selection or settings. Successful edges likewise retain selection/settings.
  /// An Error value therefore does not establish whether native cleanup remains.
  /// This value has no synchronization; shared mutation requires caller sequencing.
  class ReplicaSession final {
  public:
    /// Borrow the owned catalog snapshot, if selected; selection/clear/replacement
    /// can invalidate references into its descriptor and strings.
    [[nodiscard]] const std::optional<SourceDescriptor> &
    selected_source() const noexcept;
    /// Read visible state, which need not match the runtime's resource drain phase.
    [[nodiscard]] SessionLifecycle lifecycle() const noexcept;
    /// Borrow the current accepted settings; no platform objects are referenced.
    [[nodiscard]] const ReplicaSettings &settings() const noexcept;

    /// @brief Select a source and reset its crop/interaction without starting capture.
    void select_source(SourceDescriptor source);
    /// Remove the selected descriptor and its source-specific crop/interaction.
    void clear_source() noexcept;
    /// Store the accepted input-mode value; preference persistence belongs to Application.
    void set_mode(OverlayMode mode) noexcept;
    /// Store the accepted border preference; persistence belongs to Application.
    void set_border(BorderPreference border) noexcept;
    /// Store the accepted render cap; scheduler lifetime belongs to the runtime.
    void set_fps(FpsCap fps_cap) noexcept;
    /// Store an accepted opacity percentage; command validation belongs to Application.
    void set_opacity(int percent) noexcept;
    /// Store an accepted wheel increment; Application validates and applies it first.
    void set_wheel_zoom_percent(int percent) noexcept;
    /// Store an accepted session-only source crop; nullopt restores the full source.
    void set_region(std::optional<PixelRegion> region) noexcept;
    /// Store transient selection intent after the runtime transaction succeeds.
    void set_region_selection(bool active) noexcept;
    /// Store accepted experimental preferences without invoking platform behavior.
    void set_extensions(ExtensionSettings extensions) noexcept;

    /// @brief Enter Starting from Idle/Error when a source is selected.
    /// InvalidCommand leaves the previous state intact; source liveness must still
    /// be established by runtime construction after this domain edge succeeds.
    [[nodiscard]] std::expected<void, AppError> begin_start() noexcept;
    /// Confirm a successful start; valid only from Starting.
    [[nodiscard]] std::expected<void, AppError> mark_capturing() noexcept;
    /// @brief Enter RecoveringGraphics from Capturing, or self-transition while Interrupted.
    [[nodiscard]] std::expected<void, AppError>
    begin_graphics_recovery() noexcept;
    /// @brief Complete recovery from RecoveringGraphics or Interrupted.
    /// Only the former becomes Capturing: graphics repair cannot prove source return.
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

}    // namespace avb::core

#endif    // AVB0795_CORE_REPLICA_SESSION_HPP
