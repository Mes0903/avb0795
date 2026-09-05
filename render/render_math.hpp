#ifndef AIRELINNA_RENDER_RENDER_MATH_HPP
#define AIRELINNA_RENDER_RENDER_MATH_HPP

#include "airelinna/core/types.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <optional>
#include <span>

namespace airelinna::render {

struct Viewport final {
  std::uint32_t x{};
  std::uint32_t y{};
  std::uint32_t width{};
  std::uint32_t height{};

  friend bool operator==(const Viewport &, const Viewport &) = default;
};

[[nodiscard]] std::optional<Viewport>
aspect_fit(core::Extent source, core::Extent destination) noexcept;

[[nodiscard]] VkPresentModeKHR
select_present_mode(std::span<const VkPresentModeKHR> advertised) noexcept;

} // namespace airelinna::render

#endif // AIRELINNA_RENDER_RENDER_MATH_HPP
