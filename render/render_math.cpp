/// @file
/// @brief Integer aspect fit and explicit SDR presentation choices.
#include "render/render_math.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>

namespace avb::render {

  std::optional<Viewport> aspect_fit(const core::Extent source,
                                     const core::Extent destination) noexcept
  {
    if (source.width == 0 || source.height == 0 || destination.width == 0 ||
        destination.height == 0) {
      return std::nullopt;
    }

    // Cross multiplication chooses the limiting axis without a floating-point
    // ratio; 64-bit products cover all pairs of 32-bit input dimensions.
    const auto destination_by_source_height =
      static_cast<std::uint64_t>(destination.width) * source.height;
    const auto destination_by_source_width =
      static_cast<std::uint64_t>(destination.height) * source.width;

    std::uint32_t width{};
    std::uint32_t height{};
    if (destination_by_source_height <= destination_by_source_width) {
      width = destination.width;
      height = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(destination_by_source_height /
                                      source.width));
    }
    else {
      height = destination.height;
      width = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(destination_by_source_width /
                                      source.height));
    }

    // Truncation can differ by one pixel across opposite margins; keeping the
    // integer result contained takes precedence over exact subpixel centering.
    return Viewport{.x = (destination.width - width) / 2,
                    .y = (destination.height - height) / 2,
                    .width = width,
                    .height = height};
  }

  std::optional<RegionSampling> region_sampling(
    const std::optional<core::PixelRegion> requested, const core::Extent content,
    const core::Extent allocation, const core::Extent framebuffer) noexcept
  {
    if (content.width == 0 || content.height == 0 || content.width > allocation.width ||
        content.height > allocation.height) {
      return std::nullopt;
    }
    const core::PixelRegion full{0, 0, content.width, content.height};
    const auto region = requested
      ? core::intersect_region(*requested, content).value_or(full) : full;
    const auto viewport = aspect_fit({region.width, region.height}, framebuffer);
    if (!viewport) {
      return std::nullopt;
    }
    // Convert operands before adding the half texel, retaining fractional precision
    // until the final float conversion required by the shader push constants.
    const auto aw = static_cast<double>(allocation.width);
    const auto ah = static_cast<double>(allocation.height);
    return RegionSampling{
      .region = region, .viewport = *viewport,
      .texture_offset = {static_cast<float>(region.x / aw), static_cast<float>(region.y / ah)},
      .texture_scale = {static_cast<float>(region.width / aw), static_cast<float>(region.height / ah)},
      .texture_min = {static_cast<float>((region.x + 0.5) / aw), static_cast<float>((region.y + 0.5) / ah)},
      .texture_max = {static_cast<float>((static_cast<double>(region.x) + region.width - 0.5) / aw),
                      static_cast<float>((static_cast<double>(region.y) + region.height - 0.5) / ah)}};
  }

  std::optional<core::PixelRegion> selection_region(
    const core::Extent content, const Viewport viewport,
    const PixelPoint start, const PixelPoint end) noexcept
  {
    if (content.width == 0 || content.height == 0 || viewport.width == 0 ||
        viewport.height == 0 || !std::isfinite(start.x) || !std::isfinite(start.y) ||
        !std::isfinite(end.x) || !std::isfinite(end.y) || start.x == end.x || start.y == end.y) {
      return std::nullopt;
    }
    const auto left = static_cast<double>(viewport.x);
    const auto top = static_cast<double>(viewport.y);
    const auto right = left + viewport.width;
    const auto bottom = top + viewport.height;
    if (start.x < left || start.x >= right || start.y < top || start.y >= bottom) {
      return std::nullopt;
    }
    const PixelPoint clipped{std::clamp(end.x, left, right), std::clamp(end.y, top, bottom)};
    if (start.x == clipped.x || start.y == clipped.y) {
      return std::nullopt;
    }
    const auto source_x = [&](const double value) {
      return std::clamp((value - left) / viewport.width * content.width,
                        0.0, static_cast<double>(content.width));
    };
    const auto source_y = [&](const double value) {
      return std::clamp((value - top) / viewport.height * content.height,
                        0.0, static_cast<double>(content.height));
    };
    const auto x0 = static_cast<std::uint32_t>(std::floor(source_x((std::min)(start.x, clipped.x))));
    const auto y0 = static_cast<std::uint32_t>(std::floor(source_y((std::min)(start.y, clipped.y))));
    const auto x1 = static_cast<std::uint32_t>(std::ceil(source_x((std::max)(start.x, clipped.x))));
    const auto y1 = static_cast<std::uint32_t>(std::ceil(source_y((std::max)(start.y, clipped.y))));
    if (x1 <= x0 || y1 <= y0) {
      return std::nullopt;
    }
    return core::PixelRegion{x0, y0, x1 - x0, y1 - y0};
  }

  VkPresentModeKHR select_present_mode(
    const std::span<const VkPresentModeKHR> advertised) noexcept
  {
    if (std::ranges::find(advertised, VK_PRESENT_MODE_MAILBOX_KHR) !=
        advertised.end()) {
      return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
  }

  std::optional<VkSurfaceFormatKHR> select_sdr_surface_format(
    const std::span<const VkSurfaceFormatKHR> advertised) noexcept
  {
    // UNORM capture samples already contain SDR-encoded values. An SRGB
    // attachment would encode the passthrough shader's output a second time.
    const auto preferred = std::ranges::find_if(
      advertised, [](const VkSurfaceFormatKHR &format) noexcept {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      });
    if (preferred == advertised.end()) {
      return std::nullopt;
    }
    return *preferred;
  }

}    // namespace avb::render
