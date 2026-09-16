/// @file
/// Stable bridge-reason precedence, separated from concrete capability queries.
#include "render/bridge_policy.hpp"

namespace avb::render {

  BridgeSelection select_bridge(const BridgeCapabilityFacts facts) noexcept
  {
    // Preserve the first failed fact as the single visible explanation; later
    // optional queries must not replace a more fundamental compatibility issue.
    if (!facts.adapter_luid_matches) {
      return {core::BridgeMode::CpuCopy,
              core::BridgeReason::AdapterLuidUnavailableOrMismatch};
    }
    if (!facts.external_image_supported) {
      return {core::BridgeMode::CpuCopy,
              core::BridgeReason::ExternalImageUnsupported};
    }
    if (!facts.required_d3d11_interfaces_available) {
      return {core::BridgeMode::CpuCopy,
              core::BridgeReason::RequiredD3d11InterfacesUnavailable};
    }
    if (!facts.shared_fence_or_timeline_semaphore_available) {
      return {core::BridgeMode::CpuCopy,
              core::BridgeReason::SharedFenceOrTimelineSemaphoreUnavailable};
    }
    if (!facts.timeline_fence_import_available) {
      return {core::BridgeMode::CpuCopy,
              core::BridgeReason::TimelineFenceImportUnavailable};
    }
    if (!facts.win32_import_procedure_available) {
      return {core::BridgeMode::CpuCopy,
              core::BridgeReason::Win32ImportProcedureUnavailable};
    }
    if (!facts.compatible_memory_type_available) {
      return {core::BridgeMode::CpuCopy,
              core::BridgeReason::CompatibleMemoryTypeUnavailable};
    }
    return {core::BridgeMode::GpuInterop, core::BridgeReason::None};
  }

  std::optional<BridgeSelection>
  select_interop_fallback(const InteropFallbackPoint point) noexcept
  {
    switch (point) {
    case InteropFallbackPoint::RuntimeWorkPending:
      return std::nullopt;
    case InteropFallbackPoint::PreSubmitSetupFailure:
    case InteropFallbackPoint::RuntimeWorkRetired:
      return BridgeSelection{core::BridgeMode::CpuCopy,
                             core::BridgeReason::InteropOperationFailure};
    }
    return std::nullopt;
  }

}    // namespace avb::render
