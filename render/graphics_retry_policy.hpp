/// @file
/// Shared value policies for both Vulkan owners. Results describe evidence and
/// required actions; each concrete owner retains its own handles and timers.
#ifndef AVB0795_RENDER_GRAPHICS_RETRY_POLICY_HPP
#define AVB0795_RENDER_GRAPHICS_RETRY_POLICY_HPP

#include "avb/core/types.hpp"

#include <vulkan/vulkan_core.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace avb::render {

  /// Smallest resource scope that must be reconstructed after a native failure.
  enum class GraphicsFailureCategory { Swapchain,
                                       Surface,
                                       Device,
                                       Terminal };
  /// Independent cleanup facts; neither API's loss proves the other's work safe.
  enum class DeviceLossKind { VulkanDeviceLost,
                              D3DeviceRemoved };

  /// Requested next recovery step, without performing or owning that step.
  enum class GraphicsRetryAction {
    Retry,
    SurfaceRecreate,
    DeviceRebuild,
    Terminal,
  };

  /// Owner's attempt count, generation and elapsed time for one recovery episode.
  struct GraphicsRetryInput final {
    GraphicsFailureCategory category{GraphicsFailureCategory::Swapchain};
    std::uint32_t attempt{};
    std::uint64_t current_generation{};
    std::chrono::milliseconds elapsed_no_progress{};
  };

  /// Bounded action with candidate generation and remaining construction delay.
  struct GraphicsRetryDecision final {
    GraphicsRetryAction action{GraphicsRetryAction::Terminal};
    std::optional<std::uint64_t> candidate_generation;
    std::optional<std::chrono::milliseconds> retry_after;
  };

  /// Three construction attempts at episode offsets 0, 100 and 500 ms.
  /// The owner resets its episode only after a successful replacement present retires.
  class GraphicsRetryPolicy final {
  public:
    /// Calculate the next action; only device rebuild advances generation.
    [[nodiscard]] GraphicsRetryDecision
    evaluate(GraphicsRetryInput input) const noexcept;
  };

  /// Keep an already-due retry strictly in the future to avoid a tight loop.
  [[nodiscard]] constexpr std::chrono::milliseconds strictly_future_retry_delay(
    const std::chrono::milliseconds retry_after) noexcept
  {
    return retry_after > std::chrono::milliseconds::zero()
             ? retry_after
             : std::chrono::milliseconds{1};
  }

  /// Distinguish an unsupported loader/driver floor from other startup failures.
  [[nodiscard]] constexpr core::AppErrorCode
  vulkan_instance_create_error_code(const VkResult result) noexcept
  {
    return result == VK_ERROR_INCOMPATIBLE_DRIVER
             ? core::AppErrorCode::UnsupportedGraphics
             : core::AppErrorCode::RuntimeConstructionFailure;
  }

  /// Evidence about one swapchain's image users, presentation and failed release.
  struct SwapchainOwnershipFacts final {
    bool native_image_use_pending{};
    bool present_fence_pending{};
    bool release_retained{};
  };

  /// Replacement may retain old presents, but not unreleased acquired images.
  [[nodiscard]] constexpr bool
  same_surface_replacement_ready(const SwapchainOwnershipFacts facts) noexcept
  {
    return !facts.native_image_use_pending && !facts.release_retained;
  }

  /// Destruction requires both image-use and presentation proof. A failed image
  /// release alone does not prevent destruction once all actual use has ended.
  [[nodiscard]] constexpr bool
  retired_swapchain_destroyable(const SwapchainOwnershipFacts facts) noexcept
  {
    return !facts.native_image_use_pending && !facts.present_fence_pending;
  }

  /// How the active handle moves to the retired set before vkCreateSwapchainKHR.
  struct SameSurfaceSwapchainCreatePlan final {
    bool may_create{};
    bool retire_active_before_create{};
    bool use_active_as_old{};
  };

  /// Never reuse an old handle after a create attempt has retired it, even when
  /// creation fails; a later attempt starts with no active oldSwapchain.
  [[nodiscard]] constexpr SameSurfaceSwapchainCreatePlan
  plan_same_surface_swapchain_create(
    const bool active_present,
    const SwapchainOwnershipFacts active_ownership) noexcept
  {
    if (!active_present) {
      return {.may_create = true};
    }
    if (!same_surface_replacement_ready(active_ownership)) {
      return {};
    }
    return {.may_create = true,
            .retire_active_before_create = true,
            .use_active_as_old = true};
  }

  /// Explicitly separates WSI-only recovery from capture/device reconstruction.
  struct GraphicsRecoveryScope final {
    bool recreate_surface{};
    bool recreate_swapchain{};
    bool recreate_capture{};
    bool recreate_d3{};
    bool recreate_slots{};
    bool advance_generation{};
  };

  /// Map failure category to the resources affected; surface loss preserves
  /// capture, D3 devices, slots and the runtime-owned resource generation.
  [[nodiscard]] constexpr GraphicsRecoveryScope
  graphics_recovery_scope(const GraphicsFailureCategory category) noexcept
  {
    switch (category) {
    case GraphicsFailureCategory::Swapchain:
      return {.recreate_swapchain = true};
    case GraphicsFailureCategory::Surface:
      return {.recreate_surface = true, .recreate_swapchain = true};
    case GraphicsFailureCategory::Device:
      return {.recreate_surface = true,
              .recreate_swapchain = true,
              .recreate_capture = true,
              .recreate_d3 = true,
              .recreate_slots = true,
              .advance_generation = true};
    case GraphicsFailureCategory::Terminal:
      return {};
    }
    return {};
  }

  /// True when a replica renderer can recover without replacing capture owners.
  [[nodiscard]] constexpr bool
  render_stack_local_recovery(const GraphicsRecoveryScope scope) noexcept
  {
    return scope.recreate_swapchain && !scope.recreate_capture &&
           !scope.recreate_d3 && !scope.recreate_slots &&
           !scope.advance_generation;
  }

  /// Native proof currently owed by a ring slot. Empty can still retain a safe
  /// sampled image for redraw; it means no outstanding native use.
  enum class RenderSlotPhase {
    Empty,
    D3CopyPending,
    CpuReady,
    GpuReady,
    Submitted,
    PresentNotEnqueued,
    AcquireProofPending,
    SubmitUncertain,
    ReleaseRetained,
  };

  /// Selection-only view of a ring slot; retained is a borrowed-owner marker.
  struct RenderFrameCandidate final {
    RenderSlotPhase phase{RenderSlotPhase::Empty};
    std::uint64_t sequence{};
    bool retained{};
  };

  /// Select the newest ready sequence, then an explicitly requested retained
  /// redraw. A delayed older slot must never replace a newer submitted image.
  [[nodiscard]] constexpr std::optional<std::size_t> select_render_frame(
    const std::span<const RenderFrameCandidate> candidates,
    const std::uint64_t last_submitted_sequence,
    const bool redraw_requested) noexcept
  {
    std::optional<std::size_t> newest;
    for (std::size_t index{}; index < candidates.size(); ++index) {
      const auto &candidate = candidates[index];
      if ((candidate.phase == RenderSlotPhase::CpuReady ||
           candidate.phase == RenderSlotPhase::GpuReady) &&
          candidate.sequence > last_submitted_sequence &&
          (!newest || candidate.sequence > candidates[*newest].sequence)) {
        newest = index;
      }
    }
    if (newest || !redraw_requested) {
      return newest;
    }
    for (std::size_t index{}; index < candidates.size(); ++index) {
      const auto &candidate = candidates[index];
      if (candidate.retained && candidate.phase == RenderSlotPhase::Empty &&
          candidate.sequence >= last_submitted_sequence) {
        return index;
      }
    }
    return std::nullopt;
  }

  /// Which host-pollable signal can establish a slot's next safe cleanup step.
  enum class SlotProofFence { None,
                              Acquire,
                              Submission };

  /// An acquire without submission uses acquire proof; uncertain submission
  /// retains submission proof instead of assuming that enqueue failed.
  [[nodiscard]] constexpr SlotProofFence
  proof_fence_for_slot(const RenderSlotPhase phase) noexcept
  {
    switch (phase) {
    case RenderSlotPhase::AcquireProofPending:
      return SlotProofFence::Acquire;
    case RenderSlotPhase::Submitted:
    case RenderSlotPhase::PresentNotEnqueued:
    case RenderSlotPhase::SubmitUncertain:
      return SlotProofFence::Submission;
    case RenderSlotPhase::Empty:
    case RenderSlotPhase::D3CopyPending:
    case RenderSlotPhase::CpuReady:
    case RenderSlotPhase::GpuReady:
    case RenderSlotPhase::ReleaseRetained:
      return SlotProofFence::None;
    }
    return SlotProofFence::None;
  }

  /// Whether a slot still holds Vulkan/image ownership requiring retirement.
  [[nodiscard]] constexpr bool
  slot_has_vulkan_work(const RenderSlotPhase phase) noexcept
  {
    switch (phase) {
    case RenderSlotPhase::Submitted:
    case RenderSlotPhase::PresentNotEnqueued:
    case RenderSlotPhase::AcquireProofPending:
    case RenderSlotPhase::SubmitUncertain:
    case RenderSlotPhase::ReleaseRetained:
      return true;
    case RenderSlotPhase::Empty:
    case RenderSlotPhase::D3CopyPending:
    case RenderSlotPhase::CpuReady:
    case RenderSlotPhase::GpuReady:
      return false;
    }
    return false;
  }

  /// Cause-specific disposition of one slot during aggregate device recovery.
  enum class DeviceRecoverySlotAction {
    None,
    ClearWithoutWait,
    AwaitD3Completion,
    AwaitVulkanRetirement,
  };

  /// Preserve D3 copies after Vulkan loss and healthy Vulkan work after D3 loss.
  /// Only the corresponding confirmed loss permits that API's no-wait cleanup.
  [[nodiscard]] constexpr DeviceRecoverySlotAction
  plan_device_recovery_slot(const DeviceLossKind kind,
                            const RenderSlotPhase phase) noexcept
  {
    if (phase == RenderSlotPhase::Empty) {
      return DeviceRecoverySlotAction::None;
    }
    if (kind == DeviceLossKind::VulkanDeviceLost) {
      return phase == RenderSlotPhase::D3CopyPending
               ? DeviceRecoverySlotAction::AwaitD3Completion
               : DeviceRecoverySlotAction::ClearWithoutWait;
    }
    return slot_has_vulkan_work(phase)
             ? DeviceRecoverySlotAction::AwaitVulkanRetirement
             : DeviceRecoverySlotAction::ClearWithoutWait;
  }

  /// Owner-local retry budget; successful construction alone does not reset it.
  struct GraphicsRecoveryEpisodeState final {
    std::uint32_t attempt{};
    bool awaiting_successful_present{};
  };

  /// Events that change the existing retry budget without introducing a service.
  enum class GraphicsRecoveryObservation {
    ConstructionAttempted,
    ReplacementConstructed,
    SuccessfulPresentRetired,
  };

  /// Advance an attempted/replaced episode; reset only on proven presentation
  /// from a replacement whose successful present was still awaited.
  [[nodiscard]] constexpr GraphicsRecoveryEpisodeState
  advance_graphics_recovery_episode(
    GraphicsRecoveryEpisodeState state,
    const GraphicsRecoveryObservation observation) noexcept
  {
    switch (observation) {
    case GraphicsRecoveryObservation::ConstructionAttempted:
      if (state.attempt < (std::numeric_limits<std::uint32_t>::max)()) {
        ++state.attempt;
      }
      return state;
    case GraphicsRecoveryObservation::ReplacementConstructed:
      state.awaiting_successful_present = true;
      return state;
    case GraphicsRecoveryObservation::SuccessfulPresentRetired:
      return state.awaiting_successful_present ? GraphicsRecoveryEpisodeState{}
                                               : state;
    }
    return state;
  }

  /// Control-flow consequence of a timeout-zero swapchain acquire.
  enum class AcquireAction {
    UseImage,
    Wait,
    RecreateSwapchain,
    RecreateSurface,
    RebuildDevice,
    Terminal,
  };

  /// Whether acquire produced an image and pending signal operations.
  enum class AcquireOwnership { ImageAndSyncAcquired,
                                Unchanged };

  /// Keeps image ownership separate from suboptimal/recreation control flow.
  struct AcquireClassification final {
    AcquireAction action{AcquireAction::Terminal};
    AcquireOwnership ownership{AcquireOwnership::Unchanged};
    bool recreate_after_safe_use{};
  };

  /// Only success/suboptimal acquire an image. Other results leave the supplied
  /// semaphore/fence unchanged; timeout/not-ready request a later observation.
  [[nodiscard]] constexpr AcquireClassification
  classify_acquire_result(const VkResult result,
                          const bool full_screen_exclusive_enabled) noexcept
  {
    switch (result) {
    case VK_SUCCESS:
      return {AcquireAction::UseImage, AcquireOwnership::ImageAndSyncAcquired,
              false};
    case VK_SUBOPTIMAL_KHR:
      return {AcquireAction::UseImage, AcquireOwnership::ImageAndSyncAcquired,
              true};
    case VK_NOT_READY:
    case VK_TIMEOUT:
      return {AcquireAction::Wait, AcquireOwnership::Unchanged, false};
    case VK_ERROR_OUT_OF_DATE_KHR:
      return {AcquireAction::RecreateSwapchain, AcquireOwnership::Unchanged,
              false};
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
      return {full_screen_exclusive_enabled ? AcquireAction::RecreateSwapchain
                                            : AcquireAction::Terminal,
              AcquireOwnership::Unchanged, false};
    case VK_ERROR_SURFACE_LOST_KHR:
      return {AcquireAction::RecreateSurface, AcquireOwnership::Unchanged, false};
    case VK_ERROR_DEVICE_LOST:
      return {AcquireAction::RebuildDevice, AcquireOwnership::Unchanged, false};
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    default:
      return {AcquireAction::Terminal, AcquireOwnership::Unchanged, false};
    }
  }

  /// Submission proof obligation, including guaranteed-not-enqueued OOM.
  enum class SubmitOutcome { Enqueued,
                             NotEnqueued,
                             DeviceLost,
                             Uncertain };

  /// Unknown submission results retain possibly referenced resources; never
  /// manufacture completion by treating every failure as not enqueued.
  [[nodiscard]] constexpr SubmitOutcome
  classify_submit_result(const VkResult result) noexcept
  {
    switch (result) {
    case VK_SUCCESS:
      return SubmitOutcome::Enqueued;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return SubmitOutcome::NotEnqueued;
    case VK_ERROR_DEVICE_LOST:
      return SubmitOutcome::DeviceLost;
    default:
      return SubmitOutcome::Uncertain;
    }
  }

  /// Select the proof phase; device loss is handled by the owner's loss path.
  [[nodiscard]] constexpr std::optional<RenderSlotPhase>
  slot_phase_after_submit(const SubmitOutcome outcome) noexcept
  {
    switch (outcome) {
    case SubmitOutcome::Enqueued:
      return RenderSlotPhase::Submitted;
    case SubmitOutcome::NotEnqueued:
      return RenderSlotPhase::AcquireProofPending;
    case SubmitOutcome::Uncertain:
      return RenderSlotPhase::SubmitUncertain;
    case SubmitOutcome::DeviceLost:
      return std::nullopt;
    }
    return std::nullopt;
  }

  /// Only enabled optional extensions may explain their specialized results.
  struct PresentFeatureFacts final {
    bool full_screen_exclusive_enabled{};
    bool present_timing_enabled{};
  };

  /// Whether presentation may still reference its fence and wait semaphore.
  enum class PresentOwnership { Enqueued,
                                NotEnqueued,
                                Uncertain };
  /// Recovery scope resulting from presentation, independent of its ownership.
  enum class PresentRecovery {
    None,
    RecreateSwapchain,
    RecreateSurface,
    RebuildDevice,
    Terminal,
  };

  /// Default is conservative uncertainty: terminal does not mean safe release.
  struct PresentClassification final {
    PresentOwnership ownership{PresentOwnership::Uncertain};
    PresentRecovery recovery{PresentRecovery::Terminal};
    bool presentation_pending{true};
  };

  /// Keep WSI rejection records whose queue operations were enqueued. OOM is
  /// the known no-enqueue path; unknown results retain their present record.
  [[nodiscard]] constexpr PresentClassification
  classify_present_result(const VkResult result,
                          const PresentFeatureFacts features = {}) noexcept
  {
    switch (result) {
    case VK_SUCCESS:
      return {PresentOwnership::Enqueued, PresentRecovery::None, true};
    case VK_SUBOPTIMAL_KHR:
    case VK_ERROR_OUT_OF_DATE_KHR:
      return {PresentOwnership::Enqueued, PresentRecovery::RecreateSwapchain,
              true};
    case VK_ERROR_SURFACE_LOST_KHR:
      return {PresentOwnership::Enqueued, PresentRecovery::RecreateSurface, true};
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
      if (features.full_screen_exclusive_enabled) {
        return {PresentOwnership::Enqueued, PresentRecovery::RecreateSwapchain,
                true};
      }
      return {};
#if defined(VK_EXT_present_timing)
    case VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT:
      if (features.present_timing_enabled) {
        return {PresentOwnership::Enqueued, PresentRecovery::None, true};
      }
      return {};
#endif
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return {PresentOwnership::NotEnqueued, PresentRecovery::Terminal, false};
    case VK_ERROR_DEVICE_LOST:
      return {PresentOwnership::Uncertain, PresentRecovery::RebuildDevice, true};
    default:
      return {};
    }
  }

  /// A non-enqueued present leaves the acquired image under submission drain.
  [[nodiscard]] constexpr RenderSlotPhase
  slot_phase_after_present(const PresentOwnership ownership) noexcept
  {
    return ownership == PresentOwnership::NotEnqueued
             ? RenderSlotPhase::PresentNotEnqueued
             : RenderSlotPhase::Submitted;
  }

  /// A non-successful maintenance1 release never clears acquired-image state.
  enum class ReleaseOwnership { Released,
                                Retained,
                                Uncertain };
  /// Release failures choose surface teardown or terminal retained ownership.
  enum class ReleaseRecovery { None,
                               SurfaceLost,
                               Terminal };

  /// Paired release evidence/action; uncertainty is the fail-closed default.
  struct ReleaseClassification final {
    ReleaseOwnership ownership{ReleaseOwnership::Uncertain};
    ReleaseRecovery recovery{ReleaseRecovery::Terminal};
  };

  /// Accept only VK_SUCCESS as release proof; surface loss preserves the
  /// record under its swapchain until legal destruction is possible.
  [[nodiscard]] constexpr ReleaseClassification
  classify_release_result(const VkResult result) noexcept
  {
    switch (result) {
    case VK_SUCCESS:
      return {ReleaseOwnership::Released, ReleaseRecovery::None};
    case VK_ERROR_SURFACE_LOST_KHR:
      return {ReleaseOwnership::Retained, ReleaseRecovery::SurfaceLost};
    case VK_ERROR_VALIDATION_FAILED_EXT:
    default:
      return {ReleaseOwnership::Uncertain, ReleaseRecovery::Terminal};
    }
  }

  /// Cleanup of completed but unconsumed binary signals and image ownership.
  struct SlotReleasePlan final {
    RenderSlotPhase next_phase{RenderSlotPhase::ReleaseRetained};
    bool destroy_acquire_semaphore{};
    bool destroy_render_finished_semaphore{};
    bool clear_acquired_ownership{};
  };

  /// Determine which semaphores cannot be recycled after unused acquire/present.
  /// Image release and completed-signal destruction are separate decisions.
  [[nodiscard]] constexpr SlotReleasePlan
  plan_slot_release(const RenderSlotPhase prior_phase,
                    const ReleaseClassification classification) noexcept
  {
    const bool released = classification.ownership == ReleaseOwnership::Released;
    return {.next_phase = released ? RenderSlotPhase::Empty
                                   : RenderSlotPhase::ReleaseRetained,
            .destroy_acquire_semaphore =
              prior_phase == RenderSlotPhase::AcquireProofPending,
            .destroy_render_finished_semaphore =
              prior_phase == RenderSlotPhase::PresentNotEnqueued ||
              prior_phase == RenderSlotPhase::SubmitUncertain,
            .clear_acquired_ownership = released};
  }

  /// Distinguish pending/complete from the fence's device-removal sentinel.
  enum class D3CompletionStatus { Pending,
                                  Complete,
                                  DeviceRemoved };

  /// UINT64_MAX is removal, never proof that an arbitrary target completed.
  [[nodiscard]] constexpr D3CompletionStatus
  classify_d3_completed_value(const std::uint64_t completed,
                              const std::uint64_t target) noexcept
  {
    if (completed == (std::numeric_limits<std::uint64_t>::max)()) {
      return D3CompletionStatus::DeviceRemoved;
    }
    return completed >= target ? D3CompletionStatus::Complete
                               : D3CompletionStatus::Pending;
  }

  /// Only tracked retirement/proven release resets the no-progress timer.
  /// Polling, new work and warning edges are not forward cleanup progress.
  enum class DrainProgress {
    None,
    TrackedRetirement,
    ProvenSafeRelease,
    Poll,
    Retry,
    Warning,
    NewPending,
  };

  /// Aggregate owner evidence at one observation; elapsed time is owner-kept.
  struct RuntimeDrainInput final {
    bool pending_ownership{};
    bool ownership_uncertain{};
    DrainProgress progress{DrainProgress::None};
    std::chrono::milliseconds elapsed_no_progress{};
  };

  /// Stalled retains owners and polling; may_complete alone permits release.
  struct RuntimeDrainDecision final {
    core::RuntimeDrainStatus status{core::RuntimeDrainStatus::Normal};
    bool may_complete{};
    bool restart_no_progress_interval{};
    std::optional<std::chrono::milliseconds> poll_after;
  };

  /// Poll unresolved ownership within 50 ms; publish Stalled after ten seconds
  /// without retirement. The policy does not sleep, destroy or abandon owners.
  class RuntimeDrainPolicy final {
  public:
    /// Derive drain status/deadline from evidence without altering the owner.
    [[nodiscard]] RuntimeDrainDecision
    evaluate(RuntimeDrainInput input) const noexcept;
  };

}    // namespace avb::render

#endif    // AVB0795_RENDER_GRAPHICS_RETRY_POLICY_HPP
