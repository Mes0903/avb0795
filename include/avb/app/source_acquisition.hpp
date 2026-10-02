/// @file
/// @brief Native-free asynchronous source admission and retirement facts.
#ifndef AVB0795_APP_SOURCE_ACQUISITION_HPP
#define AVB0795_APP_SOURCE_ACQUISITION_HPP
#include "avb/core/types.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <variant>
namespace avb::app {
  /// @brief Application-issued identity; never reused within its borrow lifetime.
  struct AcquisitionId {
    std::uint64_t value{};
    friend bool operator==(const AcquisitionId &, const AcquisitionId &) = default;
  };
  /// @brief Owned start intent; missing resume identity requires fresh user selection.
  struct SourceAcquisitionRequest {
    AcquisitionId id;
    core::SessionId session_id;
    core::SourceKind kind;
    std::optional<core::SourceId> resume_source;
  };
  /// @brief Presentation phases, independent of ownership retirement.
  enum class AcquisitionPhase { Idle,
                                AwaitingPermission,
                                PreparingStream,
                                Cancelling };
  /// @brief Facts bound to the request identity; terminal facts cannot authorize stale intent.
  struct AcquisitionPhaseChanged {
    AcquisitionId id;
    AcquisitionPhase phase;
  };
  struct AcquisitionReady {
    AcquisitionId id;
    core::SourceDescriptor source;
  };
  struct AcquisitionCancelled {
    AcquisitionId id;
  };
  struct AcquisitionFailed {
    AcquisitionId id;
    core::AppError error;
  };
  using AcquisitionEvent = std::variant<AcquisitionPhaseChanged, AcquisitionReady, AcquisitionCancelled, AcquisitionFailed>;
  /// @brief Bounded results; quiescence certifies all service borrows are retired.
  /// Progress means an actual ownership acknowledgment, never a timer or phase change.
  struct SourceAcquisitionTickResult {
    std::array<std::optional<AcquisitionEvent>, 4> events{};
    std::optional<std::chrono::steady_clock::time_point> next_wake;
    bool quiescent{};
    bool made_progress{};
  };
  /// @brief Composition-owned service borrowed by Application on the main thread.
  /// Calls never reenter Application. Request arguments are borrowed only for the call;
  /// implementations retain their own values for asynchronous work. Ready certifies
  /// a prepared source available to the factory, not whole-service quiescence.
  /// Calls after cancel cannot authorize Ready publication; Cancelled/Failed acknowledge
  /// operation retirement. begin admits one request or reports typed failure;
  /// failure does not certify retirement. cancel/shutdown are nonblocking intentions.
  /// Tick until quiescent after shutdown before normal destruction; uncertain owners
  /// require the host's non-unwinding abandonment path. Native execution belongs to the concrete platform owner.
  class ISourceAcquisition {
  public:
    virtual ~ISourceAcquisition() = default;
    virtual std::expected<void, core::AppError> begin(const SourceAcquisitionRequest &) = 0;
    virtual void cancel(AcquisitionId) noexcept = 0;
    virtual void request_shutdown() noexcept = 0;
    virtual SourceAcquisitionTickResult tick(std::chrono::steady_clock::time_point) noexcept = 0;
  };
}    // namespace avb::app
#endif
