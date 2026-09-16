/// @file
/// @brief Pure bridge decisions; native capability queries and resource ownership stay
/// in WindowsRenderStack. No policy in this file performs GPU work.
#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_BRIDGE_POLICY_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_BRIDGE_POLICY_HPP

#include "avb/core/types.hpp"

#include <cstdint>
#include <optional>

namespace avb::render {

  /// @brief Construction preference; ForceCpuBaseline exercises the real CPU path.
  enum class WindowsBridgePreference { Automatic,
                                       ForceCpuBaseline };

  /// @brief Value snapshot of concrete D3D/Vulkan capability queries.
  ///
  /// False facts select a fallback reason in a stable priority order. Device-level
  /// support does not certify ring construction: compatibility is also queried
  /// for created NT handles, and subsequent import operations can still fail.
  struct BridgeCapabilityFacts final {
    bool adapter_luid_matches{};
    bool external_image_supported{};
    bool required_d3d11_interfaces_available{};
    bool shared_fence_or_timeline_semaphore_available{};
    bool timeline_fence_import_available{};
    bool win32_import_procedure_available{};
    bool compatible_memory_type_available{};
  };

  /// @brief One canonical visible bridge result; GPU interop uses BridgeReason::None.
  struct BridgeSelection final {
    BridgeSelection() = delete;
    /// @brief Store a policy-produced pair; this constructor does not validate it.
    constexpr BridgeSelection(const core::BridgeMode selected_mode,
                              const core::BridgeReason selected_reason) noexcept
        : mode{selected_mode}, reason{selected_reason} {}

    core::BridgeMode mode;
    core::BridgeReason reason;

    friend bool operator==(const BridgeSelection &,
                           const BridgeSelection &) = default;
  };

  /// @brief Evidence supplied by the owner before replacing interop resources.
  enum class InteropFallbackPoint {
    PreSubmitSetupFailure,
    RuntimeWorkPending,
    RuntimeWorkRetired,
  };

  /// @brief Select GPU interop only when all facts pass.
  ///
  /// Otherwise report the first failed fact. The returned value requests a
  /// transport; it neither allocates resources nor certifies their retirement.
  [[nodiscard]] BridgeSelection
  select_bridge(BridgeCapabilityFacts facts) noexcept;

  /// @brief Permit CPU fallback before native use or after its proven retirement.
  ///
  /// Pending work returns no selection so callers keep the current owners alive.
  /// The caller supplies the evidence; the policy does not inspect native work.
  [[nodiscard]] std::optional<BridgeSelection>
  select_interop_fallback(InteropFallbackPoint point) noexcept;

  /// @brief Suppress repeated interop probes for an already-failed CPU generation.
  ///
  /// A different resource generation may retry after prerequisite facts pass.
  /// The optional selection is borrowed only for the call; no owner is retained.
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

#endif    // AVB0795_PLATFORM_WINDOWS_DETAIL_BRIDGE_POLICY_HPP
