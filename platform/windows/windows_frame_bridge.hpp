/// @file
/// @brief Main-thread D3 producer and Windows import ownership with borrowed inputs.
#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_FRAME_BRIDGE_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_FRAME_BRIDGE_HPP

#include "avb/core/types.hpp"
#include "platform/windows/detail/bridge_policy.hpp"
#include "render/vulkan_input.hpp"

#include <dxgiformat.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

struct ID3D11Device;

namespace avb::platform::windows {

  class WindowsCapturedFrame;

  /// @brief Allocation-free events for the aggregate's existing native counters.
  /// The callback is noexcept and observes only; it must not reenter the bridge.
  enum class BridgeResourceEvent { SemaphoreCreated, SemaphoreDestroyed };
  struct BridgeResourceObserver {
    void *context{};
    void (*observe)(void *, BridgeResourceEvent) noexcept{};
  };

  /// @brief Per-handle capability probing is separate from import/setup failure.
  /// Counts include successful prefixes even when a later native create fails.
  struct InteropRingProbe {
    std::size_t created_handles{};
    std::size_t compatible_handles{};
    bool operation_failed{};
  };

  /// @brief Pending keeps a canonical reservation; SafeDiscard ends adoption safely.
  enum class BridgeSubmitDisposition { Pending, SafeDiscard };

  /// @brief Native submit outcome, independent of renderer phase and counters.
  ///
  /// Only Pending commits the supplied frame stamp and admitted-frame count.
  /// d3_copy_bytes counts an issued copy even when subsequent Signal observes
  /// removal. Apply count_as_dropped once; fallback/loss close caller admission.
  struct BridgeSubmitResult {
    BridgeSubmitDisposition disposition{BridgeSubmitDisposition::SafeDiscard};
    bool count_as_dropped{};
    bool fallback_requested{};
    bool d3_device_removed{};
    std::uint64_t d3_copy_bytes{};
  };

  /// @brief One completed producer obligation, never a mirror of renderer phase.
  enum class BridgeCompletionKind {
    ExternalReady,
    CpuConsumed,
    SafeDiscard,
    CpuConsumerFailed,
  };

  /// @brief Value handoff for a checked key after copy completion or confirmed loss.
  ///
  /// ExternalReady supplies the bridge's already-completed timeline wait.
  /// CpuConsumed means the synchronous consumer
  /// already completed the renderer transition. The other kinds permit a keyed
  /// discard, and only count_as_dropped requests the existing drop accounting.
  /// error carries a failed consumer or a native failure after safe source proof.
  struct BridgeCompletion {
    render::SlotKey key;
    BridgeCompletionKind kind{BridgeCompletionKind::SafeDiscard};
    core::Extent content{};
    render::FrameStamp stamp;
    render::CompletedProducerWait external_wait;
    bool count_as_dropped{};
    std::optional<core::AppError> error;
  };

  /// @brief At most one completed event per fixed slot, plus independent native facts.
  ///
  /// Consume only completions[0..completion_count). Native terminal_error does
  /// not discharge pending copy ownership; consumer errors instead travel in
  /// their typed completion so the caller retains their renderer provenance.
  struct BridgeProgress {
    std::array<BridgeCompletion, 3> completions{};
    std::size_t completion_count{};
    bool made_progress{};
    bool pending_inputs{};
    bool d3_device_removed{};
    bool fallback_requested{};
    std::optional<core::AppError> terminal_error;
    std::optional<std::chrono::steady_clock::time_point> next_deadline;
  };

  /// @brief Own D3 devices, source-copy slots, WGC checkouts and external Vulkan imports.
  ///
  /// All calls run on the main thread. Renderer phase, admission and allocation
  /// publication stay with the caller. Copy and move are disabled so pending
  /// producer records cannot change owner while their native work is outstanding.
  class WindowsFrameBridge final {
  public:
    /// @brief Create the hardware BGRA device; no ring or GPU work is published.
    /// Throws core::AppError on native setup failure; allocation can also throw.
    explicit WindowsFrameBridge(BridgeResourceObserver observer = {});
    /// @brief Release imports before D3 owners while the borrowed Vulkan device lives.
    /// Requires consumed producer events and detached/retired renderer borrows.
    /// An outstanding pending record terminates; destruction never waits for proof.
    ~WindowsFrameBridge() noexcept;

    WindowsFrameBridge(const WindowsFrameBridge &) = delete;
    WindowsFrameBridge &operator=(const WindowsFrameBridge &) = delete;
    WindowsFrameBridge(WindowsFrameBridge &&) = delete;
    WindowsFrameBridge &operator=(WindowsFrameBridge &&) = delete;

    /// @brief Borrow the capture device; WGC and interop users retire before this owner.
    [[nodiscard]] ID3D11Device &capture_device() noexcept;

    /// @brief Prefer the D3 adapter through a short-lived platform-neutral request.
    [[nodiscard]] render::VulkanStartupRequest startup_request() const;
    /// @brief Attach once before any ring; the device outlives this bridge.
    /// Resolves Win32 procedures and collects the initial capability prefix.
    void attach_vulkan_device(render::VulkanDeviceBorrow device) noexcept;
    [[nodiscard]] render::BridgeCapabilityFacts capability_facts() const noexcept;
    /// @brief Recreate released interop support for a new capture generation.
    /// Requires no installed ring; historical facts survive support release.
    void refresh_interop_capabilities() noexcept;
    /// @brief Probe actual-sized images, D3 textures, queries and every NT handle.
    /// Does not import memory or create views, descriptors or a timeline.
    [[nodiscard]] std::expected<InteropRingProbe, core::AppError>
    probe_interop_ring(std::uint64_t expected_serial) noexcept;
    /// @brief Import a probed slot and return a borrowed sampled-image registration.
    /// Partial native owners stay with the bridge for release_ring on failure.
    [[nodiscard]] std::expected<render::ExternalSampledImageView, core::AppError>
    finish_interop_slot(render::SlotKey key) noexcept;
    [[nodiscard]] bool ensure_interop_timeline() noexcept;
    /// @brief Release shared fence/timeline after all renderer timeline users retire.
    /// CPU rings may stay installed; an imported ring must be released first.
    /// Facts remain historical and the next fence target never moves backwards.
    void release_interop_support() noexcept;
    /// @brief Query only D3 removal; Vulkan errors never establish this fact.
    [[nodiscard]] bool d3_device_removed() const noexcept;

    /// @brief Install caller-issued identities before any partial ring allocation.
    /// Requires no installed ring, a serial newer than this owner's last install,
    /// nonzero generation and positive representable BGRA8 allocation. Failure
    /// leaves the current state unchanged; release does not reset serial history.
    [[nodiscard]] std::expected<void, core::AppError>
    start_ring(std::uint64_t serial, std::uint64_t generation,
               core::Extent allocation, DXGI_FORMAT format) noexcept;
    /// @brief Create this slot's staging texture and event query without GPU work.
    /// Partial native allocation remains owned for release_ring() on failure.
    [[nodiscard]] std::expected<void, core::AppError>
    create_cpu_slot(render::SlotKey key) noexcept;
    /// @brief Release only the matching ring after all producer events were consumed.
    /// Rejects stale serials and pending inputs before mutation. The caller also
    /// detaches descriptors/image borrows and establishes renderer retirement.
    /// Releases ring imports/staging/queries, preserving shared timeline support.
    [[nodiscard]] std::expected<void, core::AppError>
    release_ring(std::uint64_t expected_serial) noexcept;

    /// @brief Validate and adopt a checkout into a canonical caller reservation.
    ///
    /// The prepared slot selects CPU staging or bridge-owned GPU import. Errors are returned
    /// only before adoption/native copy; texture projection may also throw before
    /// adoption. InvalidValue retains the old invalid-frame drop classification.
    /// After an issued copy, Pending retains its lease even if Signal requests
    /// fallback; confirmed D3 removal instead returns SafeDiscard. The caller must
    /// preserve/cancel its matching reservation according to the returned outcome.
    [[nodiscard]] std::expected<BridgeSubmitResult, core::AppError>
    submit(render::SlotKey key, WindowsCapturedFrame &&frame,
           render::FrameStamp stamp);

    using CpuConsumer = std::function<std::expected<void, core::AppError>(
      render::SlotKey, render::CpuRowsView, render::FrameStamp)>;

    /// @brief Poll without waiting, then synchronously deliver completed mapped rows.
    ///
    /// The consumer is borrowed only during this call and must finish copy/flush
    /// before returning; rows are unmapped on every return or exception path.
    /// The consumer must not reenter this bridge or release its owning aggregate.
    /// Set discard_completed for drain, Vulkan loss or fallback: healthy pending
    /// copies still require proof, and completed inputs then skip Map/publication.
    /// A consumer failure makes the rest of this batch proof/discard-only. Apply
    /// returned events to matching canonical keys before submitting more work.
    [[nodiscard]] BridgeProgress
    progress(std::chrono::steady_clock::time_point now, bool discard_completed,
             const CpuConsumer &consume) noexcept;
    /// @brief Include Map-pending input even after its WGC checkout was returned.
    [[nodiscard]] bool has_pending_inputs() const noexcept;
    /// @brief Latch confirmed D3 removal; later progress emits each safe discard.
    /// Vulkan loss is never evidence for this operation.
    void note_d3_device_removed() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_FRAME_BRIDGE_HPP
