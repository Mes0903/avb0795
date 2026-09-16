/// @file
/// Pure bridge decisions; native capability queries and resource ownership stay
/// in WindowsRenderStack. No policy in this file performs GPU work.
#ifndef AVB0795_RENDER_BRIDGE_POLICY_HPP
#define AVB0795_RENDER_BRIDGE_POLICY_HPP

#include "avb/core/types.hpp"

#include <cstdint>
#include <optional>

namespace avb::render {

  /// Construction preference; ForceCpuBaseline exercises the real CPU path.
  enum class WindowsBridgePreference { Automatic,
                                       ForceCpuBaseline };

  /// Results of concrete D3D/Vulkan queries, including every imported NT handle.
  /// False facts select a portable fallback reason in a stable priority order.
  struct BridgeCapabilityFacts final {
    bool adapter_luid_matches{};
    bool external_image_supported{};
    bool required_d3d11_interfaces_available{};
    bool shared_fence_or_timeline_semaphore_available{};
    bool timeline_fence_import_available{};
    bool win32_import_procedure_available{};
    bool compatible_memory_type_available{};
  };

  /// One canonical visible bridge result; GPU interop uses BridgeReason::None.
  struct BridgeSelection final {
    BridgeSelection() = delete;
    /// Pair the selected transport with the reason returned by bridge policy.
    constexpr BridgeSelection(const core::BridgeMode selected_mode,
                              const core::BridgeReason selected_reason) noexcept
        : mode{selected_mode}, reason{selected_reason} {}

    core::BridgeMode mode;
    core::BridgeReason reason;

    friend bool operator==(const BridgeSelection &,
                           const BridgeSelection &) = default;
  };

  /// Whether setup failed before use or all runtime users have since retired.
  enum class InteropFallbackPoint {
    PreSubmitSetupFailure,
    RuntimeWorkPending,
    RuntimeWorkRetired,
  };

  /// Select GPU interop only when all facts pass; otherwise report the first
  /// failed fact. This decision neither allocates nor changes native owners.
  [[nodiscard]] BridgeSelection
  select_bridge(BridgeCapabilityFacts facts) noexcept;

  /// Permit operational CPU fallback only before submission or after retirement.
  /// Pending work returns no selection so callers keep the current owners alive.
  [[nodiscard]] std::optional<BridgeSelection>
  select_interop_fallback(InteropFallbackPoint point) noexcept;

  /// Avoid repeatedly probing an already-failed CPU generation; a new resource
  /// generation may retry after the prerequisite capability facts pass.
  [[nodiscard]] constexpr bool should_probe_interop_generation(
    const bool prefix_available,
    const std::optional<BridgeSelection> &current_selection,
    const std::uint64_t current_generation,
    const std::uint64_t requested_generation) noexcept
  {
    const bool cpu_fallback_selected =
      current_selection && current_selection->mode == core::BridgeMode::CpuCopy;
    return prefix_available &&
           (!cpu_fallback_selected || current_generation != requested_generation);
  }

}    // namespace avb::render

#endif    // AVB0795_RENDER_BRIDGE_POLICY_HPP
