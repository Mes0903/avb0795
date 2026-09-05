#ifndef AIRELINNA_CORE_REPLICA_SESSION_HPP
#define AIRELINNA_CORE_REPLICA_SESSION_HPP

#include "airelinna/core/types.hpp"

#include <expected>
#include <optional>

namespace airelinna::core {

  class ReplicaSession final {
  public:
    [[nodiscard]] const std::optional<SourceDescriptor> &
    selected_source() const noexcept;
    [[nodiscard]] SessionLifecycle lifecycle() const noexcept;
    [[nodiscard]] const ReplicaSettings &settings() const noexcept;

    void select_source(SourceDescriptor source);
    void clear_source() noexcept;
    void set_mode(OverlayMode mode) noexcept;
    void set_border(BorderPreference border) noexcept;
    void set_fps(FpsCap fps_cap) noexcept;

    [[nodiscard]] std::expected<void, AppError> begin_start() noexcept;
    [[nodiscard]] std::expected<void, AppError> mark_capturing() noexcept;
    [[nodiscard]] std::expected<void, AppError>
    begin_graphics_recovery() noexcept;
    [[nodiscard]] std::expected<void, AppError>
    complete_graphics_recovery() noexcept;
    [[nodiscard]] std::expected<void, AppError> mark_interrupted() noexcept;
    [[nodiscard]] std::expected<void, AppError> begin_stop() noexcept;
    [[nodiscard]] std::expected<void, AppError> complete_stop() noexcept;
    [[nodiscard]] std::expected<void, AppError> mark_error() noexcept;

  private:
    [[nodiscard]] std::expected<void, AppError>
    transition_to(SessionLifecycle target, bool allowed) noexcept;

    std::optional<SourceDescriptor> selected_source_;
    SessionLifecycle lifecycle_{SessionLifecycle::Idle};
    ReplicaSettings settings_;
  };

}    // namespace airelinna::core

#endif    // AIRELINNA_CORE_REPLICA_SESSION_HPP
