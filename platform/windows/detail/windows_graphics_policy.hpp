/// @file
/// @brief Windows aggregate recovery facts keep producer and Vulkan cleanup independent.
#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_WINDOWS_GRAPHICS_POLICY_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_WINDOWS_GRAPHICS_POLICY_HPP

#include "render/graphics_retry_policy.hpp"

#include <cstdint>
#include <limits>

namespace avb::render {

  /// @brief Independent cleanup facts; neither API's loss proves the other's work safe.
  enum class DeviceLossKind { VulkanDeviceLost,
                              D3DeviceRemoved };

  /// @brief Explicitly separates WSI-only recovery from capture/device reconstruction.
  struct GraphicsRecoveryScope final {
    bool recreate_surface{};
    bool recreate_swapchain{};
    bool recreate_capture{};
    bool recreate_d3{};
    bool recreate_slots{};
    bool advance_generation{};
  };

  /// @brief Map failure category to the resources affected; surface loss preserves
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

  /// @brief True when a replica renderer can recover without replacing capture owners.
  [[nodiscard]] constexpr bool
  render_stack_local_recovery(const GraphicsRecoveryScope scope) noexcept
  {
    return scope.recreate_swapchain && !scope.recreate_capture &&
           !scope.recreate_d3 && !scope.recreate_slots &&
           !scope.advance_generation;
  }

  /// @brief Cause-specific disposition of one slot during aggregate device recovery.
  enum class DeviceRecoverySlotAction {
    None,
    ClearWithoutWait,
    AwaitD3Completion,
    AwaitVulkanRetirement,
  };

  /// @brief Preserve D3 copies after Vulkan loss and healthy Vulkan work after D3 loss.
  /// Only the corresponding confirmed loss permits that API's no-wait cleanup.
  ///
  /// The caller must already have observed kind. Present records require their
  /// own handling; this function describes only the supplied slot phase.
  [[nodiscard]] constexpr DeviceRecoverySlotAction
  plan_device_recovery_slot(const DeviceLossKind kind,
                            const RenderSlotPhase phase) noexcept
  {
    if (phase == RenderSlotPhase::Empty) {
      return DeviceRecoverySlotAction::None;
    }
    if (kind == DeviceLossKind::VulkanDeviceLost) {
      return phase == RenderSlotPhase::ProducerPending
               ? DeviceRecoverySlotAction::AwaitD3Completion
               : DeviceRecoverySlotAction::ClearWithoutWait;
    }
    return slot_has_vulkan_work(phase)
             ? DeviceRecoverySlotAction::AwaitVulkanRetirement
             : DeviceRecoverySlotAction::ClearWithoutWait;
  }

  /// @brief Distinguish pending/complete from the fence's device-removal sentinel.
  enum class D3CompletionStatus { Pending,
                                  Complete,
                                  DeviceRemoved };

  /// @brief UINT64_MAX is removal, never proof that an arbitrary target completed.
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

}    // namespace avb::render

#endif    // AVB0795_PLATFORM_WINDOWS_DETAIL_WINDOWS_GRAPHICS_POLICY_HPP
