/// @file
/// @brief Native-free service boundaries and synchronous runtime event batches.
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

  /// Three status, five pre-stop lifecycle and two region facts fit without allocation.
  /// Stopped replaces pre-stop lifecycle candidates once teardown begins.
  inline constexpr std::size_t max_runtime_events_per_tick{10};

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
  /// Selected source disconnected after native selection cancellation; owners may remain retained.
  struct SourceLost {};
  /// Native repair began after selection cancellation; may pair with GraphicsRecovered.
  struct GraphicsRecoveryStarted {};
  /// Native repair completed; it does not establish that a lost source returned.
  struct GraphicsRecovered {};
  /// Safe teardown completed; the only event permitting published-runtime release.
  struct Stopped {};

  /// Terminal runtime failure; Application expects RuntimeStateLost from this event.
  struct Failed {
    core::AppError error;
  };

  /// @brief Current source content dimensions from a successfully retired presentation.
  /// Zero in either dimension invalidates prior bounds after source/geometry loss.
  /// Catalog and allocation sizes never establish this fact. Selection full-view
  /// transitions retain source bounds while their new presentation is pending.
  /// Changed/invalid bounds certify native selection cancellation and preference
  /// restoration; an unsafe restoration instead follows the Failed/drain contract.
  struct CaptureContentChanged {
    core::Extent extent;
  };

  /// @brief Completed source-pixel drag, or cancellation when region is nullopt.
  /// The current runtime returns at most one result per tick. Application admits
  /// it only for an active selection after lifecycle and size-change arbitration.
  /// Cancellation retains the accepted crop; explicit reset uses SetRegion instead.
  struct RegionSelectionCompleted {
    std::optional<core::PixelRegion> region;
  };

  /// Facts returned synchronously by tick, with lifecycle precedence owned by Application.
  using RuntimeEvent =
    std::variant<BridgeModeChanged, OverlayCaptureExclusionChanged, RuntimeDrainStatusChanged, OverlayCloseRequested, SourceLost, GraphicsRecoveryStarted, GraphicsRecovered, Stopped, Failed, CaptureContentChanged, RegionSelectionCompleted>;

  /// @brief Owned facts from one synchronous tick and its earliest requested wake.
  /// Slot order does not determine lifecycle priority. After stop, implementations
  /// suppress lifecycle candidates except safe Stopped; status feedback may continue.
  /// Empty slots may occur anywhere. No deadline means wait for external events,
  /// not proof of quiescence. The caller consumes this batch before ticking again;
  /// it is not an asynchronous queue requiring a separate runtime-generation token.
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

  /// @brief Discover source snapshots while native observations remain adapter-owned.
  /// Returned descriptor strings belong to the caller. An identity is a lookup
  /// token, not a liveness guarantee or a lease on the observed source.
  class ISourceCatalog {
  public:
    virtual ~ISourceCatalog() = default;
    /// @brief Enumerate into an owned snapshot or report a typed discovery failure.
    /// @pre Application calls only without a retained runtime, because refreshing
    /// native observations can invalidate the catalog identities needed by that owner.
    virtual std::expected<std::vector<core::SourceDescriptor>, core::AppError>
    enumerate() = 0;
  };

  /// @brief Main-thread control boundary for one exclusively owned native aggregate.
  /// Application retains a published instance until tick reports Stopped, even after
  /// source loss, terminal failure or stalled cleanup. Implementations must return
  /// synchronous portable facts without calling back into application mutation.
  /// request_stop() and noexcept control calls do not establish safe destruction.
  class IReplicaRuntime {
  public:
    virtual ~IReplicaRuntime() = default;
    /// @brief Apply a complete candidate before the caller commits domain settings.
    /// The borrowed candidate need only survive the call. Ordinary failure preserves
    /// verified old settings; unverifiable rollback returns RuntimeStateLost and
    /// requires safe stopping. Success is runtime acceptance, not persistence.
    /// selecting_region temporarily presents the full source in Editable mode at
    /// 100% opacity; mode/opacity/region remain the user's accepted preferences.
    virtual std::expected<void, core::AppError>
    apply(const core::ReplicaSettings &) noexcept = 0;
    /// @brief Advance native work without deliberately waiting for callback or GPU completion.
    /// Return owned facts and a steady-clock deadline for another observation;
    /// external wake delivery remains composition wiring. Continue ticking after
    /// request_stop() until Stopped or the host chooses process abandonment.
    virtual RuntimeTickResult
    tick(std::chrono::steady_clock::time_point now) noexcept = 0;
    /// Idempotently begin nonblocking drain; returning is not destruction permission.
    virtual void request_stop() noexcept = 0;
  };

  /// @brief Construct a complete runtime before transferring its exclusive ownership.
  class IReplicaRuntimeFactory {
  public:
    virtual ~IReplicaRuntimeFactory() = default;
    /// @brief Resolve the borrowed source/settings into a ready, non-null runtime.
    /// Own any source lease and retained settings needed after return; do not keep
    /// references to these caller values. On failure, clean up unpublished partial
    /// state before returning a typed error. A returned runtime immediately enters
    /// the published stop/drain lifetime contract; no app callback is retained.
    virtual std::expected<std::unique_ptr<IReplicaRuntime>, core::AppError>
    create(const core::SourceDescriptor &, const core::ReplicaSettings &) = 0;
  };

  /// @brief OS registration boundary whose activation supplies a verified chord to composition wiring.
  /// The adapter never receives session state or directly changes overlay mode.
  class IGlobalHotkey {
  public:
    virtual ~IGlobalHotkey() = default;
    /// @brief Replace the borrowed desired chord before the caller persists it.
    /// HotkeyRegistrationLost distinguishes loss of the previous binding from
    /// conflict/cleanup failure preserving it. A stored desired chord alone never
    /// certifies registration; only successful replacement establishes confirmation.
    virtual std::expected<void, core::AppError>
    replace(const core::HotkeyChord &) = 0;
    /// Disable registration during shutdown without allowing an exception to escape.
    virtual void clear() noexcept = 0;
  };

}    // namespace avb::app

#endif    // AVB0795_APP_PORTS_HPP
