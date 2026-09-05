/// @file
/// Value-only geometry and presentation choices used by the replica renderer.
#ifndef AIRELINNA_RENDER_RENDER_MATH_HPP
#define AIRELINNA_RENDER_RENDER_MATH_HPP

#include "airelinna/core/types.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <optional>
#include <span>

namespace airelinna::render {

  /// Centered physical-pixel rectangle within the destination framebuffer.
  struct Viewport final {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    friend bool operator==(const Viewport &, const Viewport &) = default;
  };

  /// Fit the entire source without stretching; return no viewport for a zero
  /// extent. Integer arithmetic keeps the result inside the destination.
  [[nodiscard]] std::optional<Viewport>
  aspect_fit(core::Extent source, core::Extent destination) noexcept;

  /// Prefer MAILBOX's newest completed frame; otherwise use required FIFO.
  [[nodiscard]] VkPresentModeKHR
  select_present_mode(std::span<const VkPresentModeKHR> advertised) noexcept;

  /// Select BGRA8 UNORM with nonlinear SDR presentation. The passthrough shader
  /// already reads encoded SDR bytes, so an SRGB attachment would encode twice.
  /// No compatible advertised pair returns nullopt instead of guessing.
  [[nodiscard]] std::optional<VkSurfaceFormatKHR>
  select_sdr_surface_format(std::span<const VkSurfaceFormatKHR> advertised) noexcept;

}    // namespace airelinna::render

#endif    // AIRELINNA_RENDER_RENDER_MATH_HPP
