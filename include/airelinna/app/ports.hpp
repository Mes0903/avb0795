#ifndef AIRELINNA_APP_PORTS_HPP
#define AIRELINNA_APP_PORTS_HPP

#include "airelinna/core/types.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

namespace airelinna::app {

  inline constexpr std::size_t max_runtime_events_per_tick{8};

  struct BridgeModeChanged {
    core::BridgeMode mode;
    core::BridgeReason reason;
  };

  struct OverlayCaptureExclusionChanged {
    core::CaptureExclusionStatus status;
  };

  struct RuntimeDrainStatusChanged {
    core::RuntimeDrainStatus status;
  };

  struct OverlayCloseRequested {};
  struct SourceLost {};
  struct GraphicsRecoveryStarted {};
  struct GraphicsRecovered {};
  struct Stopped {};

  struct Failed {
    core::AppError error;
  };

  using RuntimeEvent =
    std::variant<BridgeModeChanged, OverlayCaptureExclusionChanged, RuntimeDrainStatusChanged, OverlayCloseRequested, SourceLost, GraphicsRecoveryStarted, GraphicsRecovered, Stopped, Failed>;

  struct RuntimeTickResult {
    std::array<std::optional<RuntimeEvent>, max_runtime_events_per_tick> events{};
    std::optional<std::chrono::steady_clock::time_point> next_wake{};
  };

  static_assert(std::is_nothrow_copy_constructible_v<core::AppError>);
  static_assert(std::is_nothrow_move_constructible_v<core::AppError>);
  static_assert(std::is_nothrow_copy_constructible_v<RuntimeEvent>);
  static_assert(std::is_nothrow_move_constructible_v<RuntimeEvent>);
  static_assert(std::is_nothrow_copy_constructible_v<RuntimeTickResult>);
  static_assert(std::is_nothrow_move_constructible_v<RuntimeTickResult>);

  class ISourceCatalog {
  public:
    virtual ~ISourceCatalog() = default;
    virtual std::expected<std::vector<core::SourceDescriptor>, core::AppError>
    enumerate() = 0;
  };

  class IReplicaRuntime {
  public:
    virtual ~IReplicaRuntime() = default;
    virtual std::expected<void, core::AppError>
    apply(const core::ReplicaSettings &) noexcept = 0;
    virtual RuntimeTickResult
    tick(std::chrono::steady_clock::time_point now) noexcept = 0;
    virtual void request_stop() noexcept = 0;
  };

  class IReplicaRuntimeFactory {
  public:
    virtual ~IReplicaRuntimeFactory() = default;
    virtual std::expected<std::unique_ptr<IReplicaRuntime>, core::AppError>
    create(const core::SourceDescriptor &, const core::ReplicaSettings &) = 0;
  };

  class IGlobalHotkey {
  public:
    virtual ~IGlobalHotkey() = default;
    virtual std::expected<void, core::AppError>
    replace(const core::HotkeyChord &) = 0;
    virtual void clear() noexcept = 0;
  };

}    // namespace airelinna::app

#endif    // AIRELINNA_APP_PORTS_HPP
