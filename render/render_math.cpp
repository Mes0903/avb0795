#include "render/render_math.hpp"

#include <algorithm>
#include <cstdint>

namespace airelinna::render {

  std::optional<Viewport> aspect_fit(const core::Extent source,
                                     const core::Extent destination) noexcept
  {
    if (source.width == 0 || source.height == 0 || destination.width == 0 ||
        destination.height == 0) {
      return std::nullopt;
    }

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

    return Viewport{.x = (destination.width - width) / 2,
                    .y = (destination.height - height) / 2,
                    .width = width,
                    .height = height};
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

}    // namespace airelinna::render
