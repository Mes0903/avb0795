/// @file Native-free service boundaries and synchronous runtime event batches.
#ifndef AVB0795_APP_PORTS_HPP
#define AVB0795_APP_PORTS_HPP

#include "avb/core/types.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

namespace avb::app {

  /// Three status edges plus five pre-stop lifecycle candidates fit without allocation.
  inline constexpr std::size_t max_runtime_events_per_tick{8};

  /// Report the selected transfer path and its fallback reason as runtime status.
  struct BridgeModeChanged {
    core::BridgeMode mode;
    core::BridgeReason reason;
  };

  /// Nonfatal overlay-only capture-exclusion result; controller exclusion is separate.
  struct OverlayCaptureExclusionChanged {
    core::CaptureExclusionStatus status;
  };

  /// Whole-runtime cleanup progress edge; Stalled retains all unresolved owners.
  struct RuntimeDrainStatusChanged {
    core::RuntimeDrainStatus status;
  };

  /// User closed the overlay; Application decides the resulting lifecycle transition.
  struct OverlayCloseRequested {};
  /// Selected source disconnected; the runtime/overlay may remain retained.
  struct SourceLost {};
  /// Native repair began; may be paired with GraphicsRecovered in the same tick.
  struct GraphicsRecoveryStarted {};
  /// Native repair completed; it does not establish that a lost source returned.
  struct GraphicsRecovered {};
  /// Safe teardown completed; the only event permitting published-runtime release.
  struct Stopped {};

  /// Terminal runtime failure; Application expects RuntimeStateLost from this event.
  struct Failed {
    core::AppError error;
  };

  /// Facts returned synchronously by tick, with lifecycle precedence owned by Application.
  using RuntimeEvent =
    std::variant<BridgeModeChanged, OverlayCaptureExclusionChanged, RuntimeDrainStatusChanged, OverlayCloseRequested, SourceLost, GraphicsRecoveryStarted, GraphicsRecovered, Stopped, Failed>;

  /// Fixed optional slots plus the earliest deadline for remaining runtime work.
  /// Slot order does not determine lifecycle priority. After stop, implementations
  /// suppress lifecycle candidates except safe Stopped; status feedback may continue.
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

  /// Source discovery only; the caller owns returned descriptors, not native items.
  class ISourceCatalog {
  public:
    virtual ~ISourceCatalog() = default;
    /// Publish a current source snapshot; Application calls only without a retained runtime.
    virtual std::expected<std::vector<core::SourceDescriptor>, core::AppError>
    enumerate() = 0;
  };

  /// Coarse main-thread control boundary for one platform-owned resource aggregate.
  /// Application retains the published instance until tick reports Stopped.
  class IReplicaRuntime {
  public:
    virtual ~IReplicaRuntime() = default;
    /// Apply settings transactionally. Ordinary failure preserves verified old settings;
    /// unverifiable rollback returns RuntimeStateLost and requires safe stopping.
    virtual std::expected<void, core::AppError>
    apply(const core::ReplicaSettings &) noexcept = 0;
    /// Advance nonblocking native work and return facts/deadline without callbacks into app.
    virtual RuntimeTickResult
    tick(std::chrono::steady_clock::time_point now) noexcept = 0;
    /// Idempotently begin nonblocking drain; returning is not destruction permission.
    virtual void request_stop() noexcept = 0;
  };

  /// Construction boundary transferring exclusive ownership of a complete runtime.
  class IReplicaRuntimeFactory {
  public:
    virtual ~IReplicaRuntimeFactory() = default;
    /// Resolve selected source/settings into a ready runtime, or clean up unpublished
    /// partial state and report a typed construction failure. Retains no app callback.
    virtual std::expected<std::unique_ptr<IReplicaRuntime>, core::AppError>
    create(const core::SourceDescriptor &, const core::ReplicaSettings &) = 0;
  };

  /// OS registration boundary; activation emits commands through composition wiring.
  /// The adapter never receives session state or directly changes overlay mode.
  class IGlobalHotkey {
  public:
    virtual ~IGlobalHotkey() = default;
    /// Replace the binding transactionally; HotkeyRegistrationLost distinguishes
    /// loss of the previous binding from conflict/cleanup failure preserving it.
    virtual std::expected<void, core::AppError>
    replace(const core::HotkeyChord &) = 0;
    /// Disable registration during shutdown without allowing an exception to escape.
    virtual void clear() noexcept = 0;
  };

}    // namespace avb::app

#endif    // AVB0795_APP_PORTS_HPP
