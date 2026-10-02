/// @file
/// @brief Shared value policies for both Vulkan owners.
///
/// Results describe evidence and required actions; each concrete owner retains
/// its own handles and timers. Classifiers describe obligations without polling
/// native state, and an error category alone never establishes safe destruction.
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

  /// @brief Smallest resource scope that must be reconstructed after a native failure.
  enum class GraphicsFailureCategory { Swapchain,
                                       Surface,
                                       Device,
                                       Terminal };
  /// @brief Requested next recovery step, without performing or owning that step.
  enum class GraphicsRetryAction {
    Retry,
    SurfaceRecreate,
    DeviceRebuild,
    Terminal,
  };

  /// @brief Owner's attempt count, generation and elapsed time for one recovery episode.
  ///
  /// attempt counts construction attempts already made, starting at zero. Elapsed
  /// time is measured from the episode start; negative values are treated as zero.
  struct GraphicsRetryInput final {
    GraphicsFailureCategory category{GraphicsFailureCategory::Swapchain};
    std::uint32_t attempt{};
    std::uint64_t current_generation{};
    std::chrono::milliseconds elapsed_no_progress{};
  };

  /// @brief Bounded action with candidate generation and remaining construction delay.
  ///
  /// A terminal decision has no candidate or delay. A zero delay permits an
  /// attempt now; a positive delay is relative to the supplied observation time.
  struct GraphicsRetryDecision final {
    GraphicsRetryAction action{GraphicsRetryAction::Terminal};
    std::optional<std::uint64_t> candidate_generation;
    std::optional<std::chrono::milliseconds> retry_after;
  };

  /// @brief Three construction attempts at episode offsets 0, 100 and 500 ms.
  ///
  /// The owner retains the episode budget through replacement construction and
  /// resets it only after a successful replacement present retires. Each evaluate
  /// call is independent; this value policy stores no timer or attempt history.
  class GraphicsRetryPolicy final {
  public:
    /// @brief Calculate the next action; only device rebuild advances generation.
    ///
    /// Exhausted attempts, a terminal category, or device-generation overflow
    /// return a terminal decision. Evaluating does not consume an attempt.
    [[nodiscard]] GraphicsRetryDecision
    evaluate(GraphicsRetryInput input) const noexcept;
  };

  /// @brief Keep an already-due retry strictly in the future to avoid a tight loop.
  [[nodiscard]] constexpr std::chrono::milliseconds strictly_future_retry_delay(
    const std::chrono::milliseconds retry_after) noexcept
  {
    return retry_after > std::chrono::milliseconds::zero()
             ? retry_after
             : std::chrono::milliseconds{1};
  }

  /// @brief Distinguish an unsupported loader/driver floor from other startup failures.
  [[nodiscard]] constexpr core::AppErrorCode
  vulkan_instance_create_error_code(const VkResult result) noexcept
  {
    return result == VK_ERROR_INCOMPATIBLE_DRIVER
             ? core::AppErrorCode::UnsupportedGraphics
             : core::AppErrorCode::RuntimeConstructionFailure;
  }

  /// @brief Evidence about one swapchain's image users, presentation and failed release.
  ///
  /// The owner aggregates all slots and present records for the same handle.
  /// Submission completion can clear image use while a present fence still owes
  /// proof; release_retained records an image that could not be explicitly returned.
  struct SwapchainOwnershipFacts final {
    bool native_image_use_pending{};
    bool present_fence_pending{};
    bool release_retained{};
  };

  /// @brief Replacement may retain old presents, but not unreleased acquired images.
  [[nodiscard]] constexpr bool
  same_surface_replacement_ready(const SwapchainOwnershipFacts facts) noexcept
  {
    return !facts.native_image_use_pending && !facts.release_retained;
  }

  /// @brief Destruction requires both image-use and presentation proof. A failed image
  /// release alone does not prevent destruction once all actual use has ended.
  [[nodiscard]] constexpr bool
  retired_swapchain_destroyable(const SwapchainOwnershipFacts facts) noexcept
  {
    return !facts.native_image_use_pending && !facts.present_fence_pending;
  }

  /// @brief How the active handle moves to the retired set before vkCreateSwapchainKHR.
  struct SameSurfaceSwapchainCreatePlan final {
    bool may_create{};
    bool retire_active_before_create{};
    bool use_active_as_old{};
  };

  /// @brief Never reuse an old handle after a create attempt has retired it, even when
  /// creation fails; a later attempt starts with no active oldSwapchain.
  ///
  /// The returned plan owns no handle and changes no record. The caller performs
  /// the retirement before passing the old handle to native creation.
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

  /// @brief Native proof or retained ownership currently owed by a ring slot.
  ///
  /// Empty can still retain a safe sampled image for redraw. Ready phases have
  /// completed source copies but await selection; neither owns a producer checkout.
  /// Producer, acquire, and submission phases track distinct obligations. Present
  /// fences live in separate records and can remain pending after a slot is Empty.
  enum class RenderSlotPhase {
    Empty,
    ProducerPending,
    CpuReady,
    GpuReady,
    Submitted,
    PresentNotEnqueued,
    AcquireProofPending,
    SubmitUncertain,
    ReleaseRetained,
  };

  /// @brief Bounded present capacity is reserved before acquire, then transferred
  /// to independent present proof. A reserved use can also end without enqueue.
  enum class PresentUsePhase { Free,
                               Reserved,
                               PresentPending };

  /// @brief One use of a permanent native record; serials belong to the device
  /// owner and must survive capture-ring and swapchain replacement.
  struct PresentUseKey final {
    std::size_t index{};
    std::uint64_t serial{};
  };

  struct PresentUseState final {
    PresentUsePhase phase{PresentUsePhase::Free};
    std::uint64_t serial{};
  };

  /// @brief Reserve an unused record without wrapping its owner-wide identity.
  /// The owner first finds capacity, then treats serial exhaustion as terminal.
  [[nodiscard]] constexpr std::optional<PresentUseKey> reserve_present_use(
    PresentUseState &state, const std::size_t index,
    std::uint64_t &last_serial) noexcept
  {
    if (state.phase != PresentUsePhase::Free ||
        last_serial == (std::numeric_limits<std::uint64_t>::max)()) {
      return std::nullopt;
    }
    state = {PresentUsePhase::Reserved, ++last_serial};
    return PresentUseKey{index, state.serial};
  }

  /// @brief Validate the actual owner index, current serial and required phase.
  [[nodiscard]] constexpr bool matches_present_use(
    const PresentUseState state, const PresentUseKey key,
    const std::size_t owner_index, const PresentUsePhase expected) noexcept
  {
    return expected != PresentUsePhase::Free && key.index == owner_index &&
           key.serial != 0 && key.serial == state.serial && state.phase == expected;
  }

  /// @brief Transfer a reserved use only when present was enqueued or uncertain.
  [[nodiscard]] constexpr bool enqueue_present_use(
    PresentUseState &state, const PresentUseKey key,
    const std::size_t owner_index) noexcept
  {
    if (!matches_present_use(state, key, owner_index, PresentUsePhase::Reserved)) {
      return false;
    }
    state.phase = PresentUsePhase::PresentPending;
    return true;
  }

  /// @brief Consume an active use exactly once after its owner's native proof.
  /// Reserved uses require no-image acquire or proven submission/acquire plus
  /// release/destruction. Pending uses require the independent present fence.
  /// Clear ownership before any fallible native reset/replacement; a reset error
  /// closes admission and leaves the permanent handles for final teardown.
  [[nodiscard]] constexpr bool release_present_use(
    PresentUseState &state, const PresentUseKey key, const std::size_t owner_index,
    const PresentUsePhase expected) noexcept
  {
    if (!matches_present_use(state, key, owner_index, expected)) {
      return false;
    }
    state.phase = PresentUsePhase::Free;
    return true;
  }

  /// @brief Selection-only view of a ring slot; retained is a borrowed-owner marker.
  ///
  /// This value contains no image or lifetime lease. The native owner must keep
  /// the corresponding slot valid when applying the selected index.
  struct RenderFrameCandidate final {
    RenderSlotPhase phase{RenderSlotPhase::Empty};
    std::uint64_t sequence{};
    bool retained{};
  };

  /// @brief Select the newest ready sequence, then an explicitly requested retained redraw.
  ///
  /// A delayed older slot must never replace a newer submitted image. The span is
  /// borrowed only for the call; the returned index refers to its unchanged order.
  /// Equal ready sequences keep the first match. No eligible ready or safely empty
  /// retained candidate returns nullopt, including for an empty span.
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

  /// @brief Which host-pollable signal can establish a slot's next safe cleanup step.
  enum class SlotProofFence { None,
                              Acquire,
                              Submission };

  /// @brief An acquire without submission uses acquire proof; uncertain submission
  /// retains submission proof instead of assuming that enqueue failed.
  ///
  /// None means there is no Vulkan fence to poll for this phase, not that every
  /// API or image-ownership obligation has ended.
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
    case RenderSlotPhase::ProducerPending:
    case RenderSlotPhase::CpuReady:
    case RenderSlotPhase::GpuReady:
    case RenderSlotPhase::ReleaseRetained:
      return SlotProofFence::None;
    }
    return SlotProofFence::None;
  }

  /// @brief Whether a slot still holds Vulkan/image ownership requiring retirement.
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
    case RenderSlotPhase::ProducerPending:
    case RenderSlotPhase::CpuReady:
    case RenderSlotPhase::GpuReady:
      return false;
    }
    return false;
  }

  /// @brief Owner-local retry budget; successful construction alone does not reset it.
  struct GraphicsRecoveryEpisodeState final {
    std::uint32_t attempt{};
    bool awaiting_successful_present{};
  };

  /// @brief Events that change the existing retry budget without introducing a service.
  enum class GraphicsRecoveryObservation {
    ConstructionAttempted,
    ReplacementConstructed,
    SuccessfulPresentRetired,
  };

  /// @brief Advance an attempted/replaced episode; reset only on proven presentation
  /// from a replacement whose successful present was still awaited.
  ///
  /// The owner verifies that the present belongs to the current episode before
  /// reporting it. The returned state replaces the caller's prior value; attempted
  /// construction saturates the counter instead of wrapping to a fresh budget.
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

  /// @brief Control-flow consequence of a timeout-zero swapchain acquire.
  enum class AcquireAction {
    UseImage,
    Wait,
    RecreateSwapchain,
    RecreateSurface,
    RebuildDevice,
    Terminal,
  };

  /// @brief Whether acquire produced an image and pending signal operations.
  enum class AcquireOwnership { ImageAndSyncAcquired,
                                Unchanged };

  /// @brief Keeps image ownership separate from suboptimal/recreation control flow.
  struct AcquireClassification final {
    AcquireAction action{AcquireAction::Terminal};
    AcquireOwnership ownership{AcquireOwnership::Unchanged};
    bool recreate_after_safe_use{};
  };

  /// @brief Only success/suboptimal acquire an image. Other results leave the supplied
  /// semaphore/fence unchanged; timeout/not-ready request a later observation.
  ///
  /// ImageAndSyncAcquired records pending acquire synchronization, not host-observed
  /// completion. If no submission consumes it, acquire proof must retire first.
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

  /// @brief Submission proof obligation, including guaranteed-not-enqueued OOM.
  enum class SubmitOutcome { Enqueued,
                             NotEnqueued,
                             DeviceLost,
                             Uncertain };

  /// @brief Unknown submission results retain possibly referenced resources; never
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

  /// @brief Select the proof phase; device loss is handled by the owner's loss path.
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

  /// @brief Only enabled optional extensions may explain their specialized results.
  struct PresentFeatureFacts final {
    bool full_screen_exclusive_enabled{};
    bool present_timing_enabled{};
  };

  /// @brief Whether presentation may still reference its fence and wait semaphore.
  enum class PresentOwnership { Enqueued,
                                NotEnqueued,
                                Uncertain };
  /// @brief Recovery scope resulting from presentation, independent of its ownership.
  enum class PresentRecovery {
    None,
    RecreateSwapchain,
    RecreateSurface,
    RebuildDevice,
    Terminal,
  };

  /// @brief Default is conservative uncertainty: terminal does not mean safe release.
  struct PresentClassification final {
    PresentOwnership ownership{PresentOwnership::Uncertain};
    PresentRecovery recovery{PresentRecovery::Terminal};
    bool presentation_pending{true};
  };

  /// @brief Keep WSI rejection records whose queue operations were enqueued. OOM is
  /// the known no-enqueue path; unknown results retain their present record.
  ///
  /// Supply facts for extensions actually enabled by the device owner. Specialized
  /// results without their enabled feature stay uncertain/terminal. A recovery
  /// request does not cancel presentation_pending or transfer its ownership back.
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

  /// @brief A non-enqueued present leaves the acquired image under submission drain.
  [[nodiscard]] constexpr RenderSlotPhase
  slot_phase_after_present(const PresentOwnership ownership) noexcept
  {
    return ownership == PresentOwnership::NotEnqueued
             ? RenderSlotPhase::PresentNotEnqueued
             : RenderSlotPhase::Submitted;
  }

  /// @brief A non-successful maintenance1 release never clears acquired-image state.
  enum class ReleaseOwnership { Released,
                                Retained,
                                Uncertain };
  /// @brief Release failures choose surface teardown or terminal retained ownership.
  enum class ReleaseRecovery { None,
                               SurfaceLost,
                               Terminal };

  /// @brief Paired release evidence/action; uncertainty is the fail-closed default.
  struct ReleaseClassification final {
    ReleaseOwnership ownership{ReleaseOwnership::Uncertain};
    ReleaseRecovery recovery{ReleaseRecovery::Terminal};
  };

  /// @brief Accept only VK_SUCCESS as release proof; surface loss preserves the
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

  /// @brief Cleanup of completed but unconsumed binary signals and image ownership.
  struct SlotReleasePlan final {
    RenderSlotPhase next_phase{RenderSlotPhase::ReleaseRetained};
    bool destroy_acquire_semaphore{};
    bool destroy_render_finished_semaphore{};
    bool clear_acquired_ownership{};
  };

  /// @brief Determine which semaphores cannot be recycled after unused acquire/present.
  /// Image release and completed-signal destruction are separate decisions.
  ///
  /// Requires the prior phase's acquire/submission proof to have completed before
  /// release is attempted. This plan describes cleanup after that observation;
  /// it does not make an outstanding binary signal safe to destroy.
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

  /// @brief Only tracked retirement/proven release resets the no-progress timer.
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

  /// @brief Aggregate owner evidence at one observation; elapsed time is owner-kept.
  ///
  /// ownership_uncertain prevents completion even if no known pending record is
  /// counted. New work and repeated polls must not restart elapsed_no_progress.
  struct RuntimeDrainInput final {
    bool pending_ownership{};
    bool ownership_uncertain{};
    DrainProgress progress{DrainProgress::None};
    std::chrono::milliseconds elapsed_no_progress{};
  };

  /// @brief Stalled retains owners and polling; may_complete alone permits release.
  struct RuntimeDrainDecision final {
    core::RuntimeDrainStatus status{core::RuntimeDrainStatus::Normal};
    bool may_complete{};
    bool restart_no_progress_interval{};
    std::optional<std::chrono::milliseconds> poll_after;
  };

  /// @brief Schedule unresolved ownership checks within 50 ms and detect stalled drains.
  ///
  /// Ten seconds without retirement yields Stalled while retaining the polling
  /// obligation. The policy does not sleep, destroy or abandon owners; the caller
  /// keeps the no-progress clock and applies a requested restart on real progress.
  class RuntimeDrainPolicy final {
  public:
    /// @brief Derive drain status/deadline from evidence without altering the owner.
    [[nodiscard]] RuntimeDrainDecision
    evaluate(RuntimeDrainInput input) const noexcept;
  };

}    // namespace avb::render

#endif    // AVB0795_RENDER_GRAPHICS_RETRY_POLICY_HPP
