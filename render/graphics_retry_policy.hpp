#ifndef AIRELINNA_RENDER_GRAPHICS_RETRY_POLICY_HPP
#define AIRELINNA_RENDER_GRAPHICS_RETRY_POLICY_HPP

#include "airelinna/core/types.hpp"

#include <vulkan/vulkan_core.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace airelinna::render {

enum class GraphicsFailureCategory { Swapchain, Surface, Device, Terminal };
enum class DeviceLossKind { VulkanDeviceLost, D3DeviceRemoved };

enum class GraphicsRetryAction {
  Retry,
  SurfaceRecreate,
  DeviceRebuild,
  Terminal,
};

struct GraphicsRetryInput final {
  GraphicsFailureCategory category{GraphicsFailureCategory::Swapchain};
  std::uint32_t attempt{};
  std::uint64_t current_generation{};
  std::chrono::milliseconds elapsed_no_progress{};
};

struct GraphicsRetryDecision final {
  GraphicsRetryAction action{GraphicsRetryAction::Terminal};
  std::optional<std::uint64_t> candidate_generation;
  std::optional<std::chrono::milliseconds> retry_after;
};

class GraphicsRetryPolicy final {
public:
  [[nodiscard]] GraphicsRetryDecision
  evaluate(GraphicsRetryInput input) const noexcept;
};

[[nodiscard]] constexpr std::chrono::milliseconds strictly_future_retry_delay(
    const std::chrono::milliseconds retry_after) noexcept {
  return retry_after > std::chrono::milliseconds::zero()
             ? retry_after
             : std::chrono::milliseconds{1};
}

[[nodiscard]] constexpr core::AppErrorCode
vulkan_instance_create_error_code(const VkResult result) noexcept {
  return result == VK_ERROR_INCOMPATIBLE_DRIVER
             ? core::AppErrorCode::UnsupportedGraphics
             : core::AppErrorCode::RuntimeConstructionFailure;
}

struct SwapchainOwnershipFacts final {
  bool native_image_use_pending{};
  bool present_fence_pending{};
  bool release_retained{};
};

[[nodiscard]] constexpr bool
same_surface_replacement_ready(const SwapchainOwnershipFacts facts) noexcept {
  return !facts.native_image_use_pending && !facts.release_retained;
}

[[nodiscard]] constexpr bool
retired_swapchain_destroyable(const SwapchainOwnershipFacts facts) noexcept {
  return !facts.native_image_use_pending && !facts.present_fence_pending;
}

struct SameSurfaceSwapchainCreatePlan final {
  bool may_create{};
  bool retire_active_before_create{};
  bool use_active_as_old{};
};

[[nodiscard]] constexpr SameSurfaceSwapchainCreatePlan
plan_same_surface_swapchain_create(
    const bool active_present,
    const SwapchainOwnershipFacts active_ownership) noexcept {
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

struct GraphicsRecoveryScope final {
  bool recreate_surface{};
  bool recreate_swapchain{};
  bool recreate_capture{};
  bool recreate_d3{};
  bool recreate_slots{};
  bool advance_generation{};
};

[[nodiscard]] constexpr GraphicsRecoveryScope
graphics_recovery_scope(const GraphicsFailureCategory category) noexcept {
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

[[nodiscard]] constexpr bool
render_stack_local_recovery(const GraphicsRecoveryScope scope) noexcept {
  return scope.recreate_swapchain && !scope.recreate_capture &&
         !scope.recreate_d3 && !scope.recreate_slots &&
         !scope.advance_generation;
}

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

enum class SlotProofFence { None, Acquire, Submission };

[[nodiscard]] constexpr SlotProofFence
proof_fence_for_slot(const RenderSlotPhase phase) noexcept {
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

[[nodiscard]] constexpr bool
slot_has_vulkan_work(const RenderSlotPhase phase) noexcept {
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

enum class DeviceRecoverySlotAction {
  None,
  ClearWithoutWait,
  AwaitD3Completion,
  AwaitVulkanRetirement,
};

[[nodiscard]] constexpr DeviceRecoverySlotAction
plan_device_recovery_slot(const DeviceLossKind kind,
                          const RenderSlotPhase phase) noexcept {
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

struct GraphicsRecoveryEpisodeState final {
  std::uint32_t attempt{};
  bool awaiting_successful_present{};
};

enum class GraphicsRecoveryObservation {
  ConstructionAttempted,
  ReplacementConstructed,
  SuccessfulPresentRetired,
};

[[nodiscard]] constexpr GraphicsRecoveryEpisodeState
advance_graphics_recovery_episode(
    GraphicsRecoveryEpisodeState state,
    const GraphicsRecoveryObservation observation) noexcept {
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

enum class AcquireAction {
  UseImage,
  Wait,
  RecreateSwapchain,
  RecreateSurface,
  RebuildDevice,
  Terminal,
};

enum class AcquireOwnership { ImageAndSyncAcquired, Unchanged };

struct AcquireClassification final {
  AcquireAction action{AcquireAction::Terminal};
  AcquireOwnership ownership{AcquireOwnership::Unchanged};
  bool recreate_after_safe_use{};
};

[[nodiscard]] constexpr AcquireClassification
classify_acquire_result(const VkResult result,
                        const bool full_screen_exclusive_enabled) noexcept {
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

enum class SubmitOutcome { Enqueued, NotEnqueued, DeviceLost, Uncertain };

[[nodiscard]] constexpr SubmitOutcome
classify_submit_result(const VkResult result) noexcept {
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

[[nodiscard]] constexpr std::optional<RenderSlotPhase>
slot_phase_after_submit(const SubmitOutcome outcome) noexcept {
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

struct PresentFeatureFacts final {
  bool full_screen_exclusive_enabled{};
  bool present_timing_enabled{};
};

enum class PresentOwnership { Enqueued, NotEnqueued, Uncertain };
enum class PresentRecovery {
  None,
  RecreateSwapchain,
  RecreateSurface,
  RebuildDevice,
  Terminal,
};

struct PresentClassification final {
  PresentOwnership ownership{PresentOwnership::Uncertain};
  PresentRecovery recovery{PresentRecovery::Terminal};
  bool presentation_pending{true};
};

[[nodiscard]] constexpr PresentClassification
classify_present_result(const VkResult result,
                        const PresentFeatureFacts features = {}) noexcept {
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

[[nodiscard]] constexpr RenderSlotPhase
slot_phase_after_present(const PresentOwnership ownership) noexcept {
  return ownership == PresentOwnership::NotEnqueued
             ? RenderSlotPhase::PresentNotEnqueued
             : RenderSlotPhase::Submitted;
}

enum class ReleaseOwnership { Released, Retained, Uncertain };
enum class ReleaseRecovery { None, SurfaceLost, Terminal };

struct ReleaseClassification final {
  ReleaseOwnership ownership{ReleaseOwnership::Uncertain};
  ReleaseRecovery recovery{ReleaseRecovery::Terminal};
};

[[nodiscard]] constexpr ReleaseClassification
classify_release_result(const VkResult result) noexcept {
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

struct SlotReleasePlan final {
  RenderSlotPhase next_phase{RenderSlotPhase::ReleaseRetained};
  bool destroy_acquire_semaphore{};
  bool destroy_render_finished_semaphore{};
  bool clear_acquired_ownership{};
};

[[nodiscard]] constexpr SlotReleasePlan
plan_slot_release(const RenderSlotPhase prior_phase,
                  const ReleaseClassification classification) noexcept {
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

enum class D3CompletionStatus { Pending, Complete, DeviceRemoved };

[[nodiscard]] constexpr D3CompletionStatus
classify_d3_completed_value(const std::uint64_t completed,
                            const std::uint64_t target) noexcept {
  if (completed == (std::numeric_limits<std::uint64_t>::max)()) {
    return D3CompletionStatus::DeviceRemoved;
  }
  return completed >= target ? D3CompletionStatus::Complete
                             : D3CompletionStatus::Pending;
}

enum class DrainProgress {
  None,
  TrackedRetirement,
  ProvenSafeRelease,
  Poll,
  Retry,
  Warning,
  NewPending,
};

struct RuntimeDrainInput final {
  bool pending_ownership{};
  bool ownership_uncertain{};
  DrainProgress progress{DrainProgress::None};
  std::chrono::milliseconds elapsed_no_progress{};
};

struct RuntimeDrainDecision final {
  core::RuntimeDrainStatus status{core::RuntimeDrainStatus::Normal};
  bool may_complete{};
  bool restart_no_progress_interval{};
  std::optional<std::chrono::milliseconds> poll_after;
};

class RuntimeDrainPolicy final {
public:
  [[nodiscard]] RuntimeDrainDecision
  evaluate(RuntimeDrainInput input) const noexcept;
};

} // namespace airelinna::render

#endif // AIRELINNA_RENDER_GRAPHICS_RETRY_POLICY_HPP
