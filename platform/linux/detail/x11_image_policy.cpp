/// @file
/// @brief All-or-nothing layout admission and explicit pixel-word normalization.
#include "platform/linux/detail/x11_image_policy.hpp"
#include <bit>
#include <limits>
namespace avb::linux_platform::detail {
  namespace {
    bool row_span(std::size_t pitch, std::size_t width, std::size_t height, std::size_t &span) noexcept
    {
      const auto max = std::numeric_limits<std::size_t>::max();
      if (!width || !height || width > max / 4) return false;
      const auto row = width * 4;
      if (pitch < row || height - 1 > (max - row) / pitch) return false;
      span = (height - 1) * pitch + row;
      return true;
    }
    bool channel(std::uint32_t mask) noexcept
    {
      return mask && (mask >> std::countr_zero(mask)) == 255;
    }
  }
  std::optional<X11ImagePlan> inspect_x11_image(const X11ImageFacts &f, std::span<const std::byte> payload) noexcept
  {
    if (!f.true_color || (f.depth != 24 && f.depth != 32) || f.bits_per_pixel != 32 ||
        (f.scanline_pad != 8 && f.scanline_pad != 16 && f.scanline_pad != 32) ||
        (f.byte_order != X11ByteOrder::LeastSignificant && f.byte_order != X11ByteOrder::MostSignificant) ||
        !channel(f.red_mask) || !channel(f.green_mask) || !channel(f.blue_mask) ||
        (f.red_mask & f.green_mask) || (f.red_mask & f.blue_mask) || (f.green_mask & f.blue_mask) ||
        (f.depth == 24 && ((f.red_mask | f.green_mask | f.blue_mask) & 0xff000000U))) return {};
    if (f.region.width != f.allocation.width || f.region.height != f.allocation.height ||
        f.region.x > f.drawable_extent.width || f.region.y > f.drawable_extent.height ||
        f.region.width > f.drawable_extent.width - f.region.x ||
        f.region.height > f.drawable_extent.height - f.region.y || f.stride % (f.scanline_pad / 8)) return {};
    std::size_t span{};
    if (!row_span(f.stride, f.allocation.width, f.allocation.height, span) || span > payload.size()) return {};
    return X11ImagePlan{f, payload};
  }
  bool copy_x11_image(const X11ImagePlan &plan, std::span<std::byte> destination, std::size_t pitch) noexcept
  {
    if (!inspect_x11_image(plan.facts, plan.payload)) return false;
    const auto &f = plan.facts;
    std::size_t span{};
    if (!row_span(pitch, f.allocation.width, f.allocation.height, span) || span > destination.size()) return false;
    const auto source = reinterpret_cast<std::uintptr_t>(plan.payload.data());
    const auto target = reinterpret_cast<std::uintptr_t>(destination.data());
    const auto max = std::numeric_limits<std::uintptr_t>::max();
    if (plan.payload.size() > max - source || destination.size() > max - target ||
        (source < target + destination.size() && target < source + plan.payload.size())) return false;
    const auto r = std::countr_zero(f.red_mask), g = std::countr_zero(f.green_mask), b = std::countr_zero(f.blue_mask);
    for (std::size_t y = 0; y < f.allocation.height; ++y) {
      for (std::size_t x = 0; x < f.allocation.width; ++x) {
        const auto *in = plan.payload.data() + y * f.stride + x * 4;
        std::uint32_t word{};
        for (unsigned i = 0; i < 4; ++i) {
          const auto shift = f.byte_order == X11ByteOrder::LeastSignificant ? i * 8 : (3 - i) * 8;
          word |= std::to_integer<std::uint32_t>(in[i]) << shift;
        }
        auto *out = destination.data() + y * pitch + x * 4;
        out[0] = std::byte((word & f.blue_mask) >> b);
        out[1] = std::byte((word & f.green_mask) >> g);
        out[2] = std::byte((word & f.red_mask) >> r);
        out[3] = std::byte{255};
      }
    }
    return true;
  }
}
