/// @file Domain transition guards; no operation acquires or releases native owners.
#include "airelinna/core/replica_session.hpp"

#include <utility>

namespace airelinna::core {

  const std::optional<SourceDescriptor> &
  ReplicaSession::selected_source() const noexcept
  {
    return selected_source_;
  }

  SessionLifecycle ReplicaSession::lifecycle() const noexcept
  {
    return lifecycle_;
  }

  const ReplicaSettings &ReplicaSession::settings() const noexcept
  {
    return settings_;
  }

  void ReplicaSession::select_source(SourceDescriptor source)
  {
    selected_source_ = std::move(source);
  }

  void ReplicaSession::clear_source() noexcept { selected_source_.reset(); }

  void ReplicaSession::set_mode(OverlayMode mode) noexcept
  {
    settings_.mode = mode;
  }

  void ReplicaSession::set_border(BorderPreference border) noexcept
  {
    settings_.border = border;
  }

  void ReplicaSession::set_fps(FpsCap fps_cap) noexcept
  {
    settings_.fps_cap = fps_cap;
  }

  std::expected<void, AppError> ReplicaSession::begin_start() noexcept
  {
    const bool allowed =
      selected_source_.has_value() && (lifecycle_ == SessionLifecycle::Idle ||
                                       lifecycle_ == SessionLifecycle::Error);
    return transition_to(SessionLifecycle::Starting, allowed);
  }

  std::expected<void, AppError> ReplicaSession::mark_capturing() noexcept
  {
    return transition_to(SessionLifecycle::Capturing,
                         lifecycle_ == SessionLifecycle::Starting);
  }

  std::expected<void, AppError>
  ReplicaSession::begin_graphics_recovery() noexcept
  {
    const bool allowed = lifecycle_ == SessionLifecycle::Capturing ||
                         lifecycle_ == SessionLifecycle::Interrupted;
    // Graphics can repair while the retained source remains disconnected.
    const auto target = lifecycle_ == SessionLifecycle::Interrupted
                          ? SessionLifecycle::Interrupted
                          : SessionLifecycle::RecoveringGraphics;
    return transition_to(target, allowed);
  }

  std::expected<void, AppError>
  ReplicaSession::complete_graphics_recovery() noexcept
  {
    // Successful graphics repair is not evidence that source capture resumed.
    const bool allowed = lifecycle_ == SessionLifecycle::RecoveringGraphics ||
                         lifecycle_ == SessionLifecycle::Interrupted;
    const auto target = lifecycle_ == SessionLifecycle::Interrupted
                          ? SessionLifecycle::Interrupted
                          : SessionLifecycle::Capturing;
    return transition_to(target, allowed);
  }

  std::expected<void, AppError> ReplicaSession::mark_interrupted() noexcept
  {
    const bool allowed = lifecycle_ == SessionLifecycle::Capturing ||
                         lifecycle_ == SessionLifecycle::RecoveringGraphics ||
                         lifecycle_ == SessionLifecycle::Interrupted;
    return transition_to(SessionLifecycle::Interrupted, allowed);
  }

  std::expected<void, AppError> ReplicaSession::begin_stop() noexcept
  {
    const bool allowed = lifecycle_ == SessionLifecycle::Capturing ||
                         lifecycle_ == SessionLifecycle::RecoveringGraphics ||
                         lifecycle_ == SessionLifecycle::Interrupted;
    return transition_to(SessionLifecycle::Stopping, allowed);
  }

  std::expected<void, AppError> ReplicaSession::complete_stop() noexcept
  {
    // Selection/settings survive ordinary stop; Error-preserving cleanup is
    // handled by Application without routing through this Stopping-only edge.
    return transition_to(SessionLifecycle::Idle,
                         lifecycle_ == SessionLifecycle::Stopping);
  }

  std::expected<void, AppError> ReplicaSession::mark_error() noexcept
  {
    const bool allowed = lifecycle_ == SessionLifecycle::Starting ||
                         lifecycle_ == SessionLifecycle::Capturing ||
                         lifecycle_ == SessionLifecycle::RecoveringGraphics ||
                         lifecycle_ == SessionLifecycle::Interrupted ||
                         lifecycle_ == SessionLifecycle::Error;
    return transition_to(SessionLifecycle::Error, allowed);
  }

  std::expected<void, AppError>
  ReplicaSession::transition_to(SessionLifecycle target, bool allowed) noexcept
  {
    if (!allowed) {
      return std::unexpected(AppError{AppErrorCode::InvalidCommand});
    }

    lifecycle_ = target;
    return {};
  }

}    // namespace airelinna::core
