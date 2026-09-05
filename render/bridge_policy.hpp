#ifndef AIRELINNA_RENDER_BRIDGE_POLICY_HPP
#define AIRELINNA_RENDER_BRIDGE_POLICY_HPP

#include "airelinna/core/types.hpp"

#include <cstdint>
#include <optional>

namespace airelinna::render {

struct BridgeCapabilityFacts final {
  bool adapter_luid_matches{};
  bool external_image_supported{};
  bool required_d3d11_interfaces_available{};
  bool shared_fence_or_timeline_semaphore_available{};
  bool timeline_fence_import_available{};
  bool win32_import_procedure_available{};
  bool compatible_memory_type_available{};
};

struct BridgeSelection final {
  BridgeSelection() = delete;
  constexpr BridgeSelection(const core::BridgeMode selected_mode,
                            const core::BridgeReason selected_reason) noexcept
      : mode{selected_mode}, reason{selected_reason} {}

  core::BridgeMode mode;
  core::BridgeReason reason;

  friend bool operator==(const BridgeSelection &,
                         const BridgeSelection &) = default;
};

enum class InteropFallbackPoint {
  PreSubmitSetupFailure,
  RuntimeWorkPending,
  RuntimeWorkRetired,
};

[[nodiscard]] BridgeSelection
select_bridge(BridgeCapabilityFacts facts) noexcept;

[[nodiscard]] std::optional<BridgeSelection>
select_interop_fallback(InteropFallbackPoint point) noexcept;

[[nodiscard]] constexpr bool should_probe_interop_generation(
    const bool prefix_available,
    const std::optional<BridgeSelection> &current_selection,
    const std::uint64_t current_generation,
    const std::uint64_t requested_generation) noexcept {
  const bool cpu_fallback_selected =
      current_selection && current_selection->mode == core::BridgeMode::CpuCopy;
  return prefix_available &&
         (!cpu_fallback_selected || current_generation != requested_generation);
}

} // namespace airelinna::render

#endif // AIRELINNA_RENDER_BRIDGE_POLICY_HPP
