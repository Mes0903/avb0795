/// @file
/// @brief Value-only geometry and presentation choices used by the replica renderer.
#ifndef AVB0795_RENDER_RENDER_MATH_HPP
#define AVB0795_RENDER_RENDER_MATH_HPP

#include "avb/core/types.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <array>
#include <optional>
#include <span>

namespace avb::render {

  /// @brief Physical-pixel rectangle produced by aspect_fit().
  ///
  /// The origin is relative to the destination framebuffer. Aggregate construction
  /// does not itself enforce containment or centering.
  struct Viewport final {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    friend bool operator==(const Viewport &, const Viewport &) = default;
  };

  /// @brief Fit source content inside a destination using a common scale.
  ///
  /// Any zero dimension returns nullopt. The limiting axis fills the destination;
  /// the other truncates to whole pixels with a one-pixel minimum. Centering puts
  /// an odd spare pixel on the right or bottom, so extreme ratios are approximate.
  [[nodiscard]] std::optional<Viewport>
  aspect_fit(core::Extent source, core::Extent destination) noexcept;

  /// @brief Position measured in physical destination-framebuffer pixels.
  /// Fractional values retain precision when a native client coordinate is scaled
  /// to the framebuffer. The runtime owns that client-to-framebuffer step.
  struct PixelPoint {
    double x{};
    double y{};
    friend bool operator==(const PixelPoint &, const PixelPoint &) = default;
  };

  /// @brief One draw's effective source rectangle, destination and texture transform.
  /// Offsets/scales are allocation-normalized; min/max address selected texel
  /// centers. The latter constrain linear filtering at the crop boundary so pixels
  /// outside the region and capture-allocation padding stay outside the sample.
  struct RegionSampling {
    core::PixelRegion region;
    Viewport viewport;
    std::array<float, 2> texture_offset;
    std::array<float, 2> texture_scale;
    std::array<float, 2> texture_min;
    std::array<float, 2> texture_max;
  };

  /// @brief Derive aspect-fit geometry and UV bounds from one frame's actual content.
  /// Missing or empty-intersection regions select full content. Invalid extents or
  /// content exceeding allocation return nullopt. Copies of this result can be
  /// attached to an in-flight draw independently of later presentation edits.
  [[nodiscard]] std::optional<RegionSampling>
  region_sampling(std::optional<core::PixelRegion> requested, core::Extent content,
                  core::Extent allocation, core::Extent framebuffer) noexcept;

  /// @brief Convert a drag over a full-source viewport to source-pixel coverage.
  /// The start must lie inside content; a later endpoint may extend beyond it and
  /// is clipped. Normalize reverse drags, floor the top/left and ceil bottom/right
  /// to cover touched source pixels. Empty drags or non-finite coordinates return
  /// nullopt. Callers keep the mapping fixed throughout a gesture.
  [[nodiscard]] std::optional<core::PixelRegion>
  selection_region(core::Extent content, Viewport viewport,
                   PixelPoint start, PixelPoint end) noexcept;

  /// @brief Prefer advertised MAILBOX; otherwise select the FIFO fallback.
  ///
  /// The span is borrowed for this call. An empty span also returns FIFO;
  /// advertisement validity remains the native caller's responsibility.
  [[nodiscard]] VkPresentModeKHR
  select_present_mode(std::span<const VkPresentModeKHR> advertised) noexcept;

  /// @brief Select the exact BGRA8 UNORM/nonlinear SDR surface-format pair.
  ///
  /// The passthrough shader reads encoded SDR bytes, so an SRGB attachment would
  /// encode twice. Return a copied value from the borrowed span, or nullopt when
  /// absent; undefined-format advertisements are not expanded into candidates.
  [[nodiscard]] std::optional<VkSurfaceFormatKHR>
  select_sdr_surface_format(std::span<const VkSurfaceFormatKHR> advertised) noexcept;

}    // namespace avb::render

#endif    // AVB0795_RENDER_RENDER_MATH_HPP
