#include "platform/windows/detail/overlay_policy.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace airelinna::platform::windows::detail {
namespace {

[[nodiscard]] std::int64_t
rounded_ratio(const std::int64_t value, const std::uint32_t numerator,
              const std::uint32_t denominator) noexcept {
  if (value <= 0 || numerator == 0 || denominator == 0) {
    return value;
  }
  const auto scaled =
      static_cast<std::uint64_t>(value) * numerator + denominator / 2U;
  return static_cast<std::int64_t>(
      (std::min)(scaled / denominator,
                 static_cast<std::uint64_t>(
                     (std::numeric_limits<std::int32_t>::max)())));
}

[[nodiscard]] constexpr std::int32_t
saturate_coordinate(const std::int64_t value) noexcept {
  return static_cast<std::int32_t>(
      (std::clamp)(value,
                   static_cast<std::int64_t>(
                       (std::numeric_limits<std::int32_t>::min)()),
                   static_cast<std::int64_t>(
                       (std::numeric_limits<std::int32_t>::max)())));
}

struct AxisRange final {
  std::int32_t first{};
  std::int32_t last{};
};

[[nodiscard]] AxisRange clamp_axis(const std::int32_t first,
                                   const std::int32_t last,
                                   const std::int32_t work_first,
                                   const std::int32_t work_last) noexcept {
  const auto available = static_cast<std::int64_t>(work_last) - work_first;
  if (available <= 0) {
    return {.first = work_first, .last = work_first};
  }
  const auto requested =
      (std::max)(static_cast<std::int64_t>(last) - first, std::int64_t{0});
  if (requested >= available) {
    return {.first = work_first, .last = work_last};
  }
  const auto latest_first = static_cast<std::int64_t>(work_last) - requested;
  const auto clamped_first =
      (std::clamp)(static_cast<std::int64_t>(first),
                   static_cast<std::int64_t>(work_first), latest_first);
  return {.first = saturate_coordinate(clamped_first),
          .last = saturate_coordinate(clamped_first + requested)};
}

[[nodiscard]] bool moves_left(const SizingEdge edge) noexcept {
  return edge == SizingEdge::Left || edge == SizingEdge::TopLeft ||
         edge == SizingEdge::BottomLeft;
}

[[nodiscard]] bool moves_top(const SizingEdge edge) noexcept {
  return edge == SizingEdge::Top || edge == SizingEdge::TopLeft ||
         edge == SizingEdge::TopRight;
}

[[nodiscard]] bool is_vertical_edge(const SizingEdge edge) noexcept {
  return edge == SizingEdge::Top || edge == SizingEdge::Bottom;
}

} // namespace

HitZone overlay_hit_test(const core::OverlayMode mode,
                         const core::BorderPreference border,
                         const NativeRect window, const NativePoint cursor,
                         const std::uint32_t resize_band) noexcept {
  if (mode == core::OverlayMode::PassThrough) {
    return HitZone::Transparent;
  }
  if (border == core::BorderPreference::Bordered) {
    return HitZone::Delegate;
  }

  const auto width =
      (std::max)(static_cast<std::int64_t>(window.right) - window.left,
                 std::int64_t{0});
  const auto height =
      (std::max)(static_cast<std::int64_t>(window.bottom) - window.top,
                 std::int64_t{0});
  const auto band = (std::min)({width / 2, height / 2,
                                static_cast<std::int64_t>(resize_band)});
  const bool left = static_cast<std::int64_t>(cursor.x) < window.left + band;
  const bool right = static_cast<std::int64_t>(cursor.x) >= window.right - band;
  const bool top = static_cast<std::int64_t>(cursor.y) < window.top + band;
  const bool bottom =
      static_cast<std::int64_t>(cursor.y) >= window.bottom - band;

  if (top && left) {
    return HitZone::TopLeft;
  }
  if (top && right) {
    return HitZone::TopRight;
  }
  if (bottom && left) {
    return HitZone::BottomLeft;
  }
  if (bottom && right) {
    return HitZone::BottomRight;
  }
  if (left) {
    return HitZone::Left;
  }
  if (right) {
    return HitZone::Right;
  }
  if (top) {
    return HitZone::Top;
  }
  if (bottom) {
    return HitZone::Bottom;
  }
  return HitZone::Caption;
}

std::uint32_t scaled_resize_band(const std::uint32_t logical_pixels,
                                 const std::uint32_t dpi) noexcept {
  constexpr std::uint64_t default_dpi{96};
  const auto scaled =
      static_cast<std::uint64_t>(logical_pixels) * dpi + default_dpi / 2U;
  return static_cast<std::uint32_t>(
      (std::min)(scaled / default_dpi,
                 static_cast<std::uint64_t>(
                     (std::numeric_limits<std::uint32_t>::max)())));
}

NativeRect aspect_constrained_rect(const NativeRect proposed,
                                   const SizingEdge edge,
                                   const core::Extent source,
                                   const FrameInsets insets) noexcept {
  const auto proposed_width =
      static_cast<std::int64_t>(proposed.right) - proposed.left;
  const auto proposed_height =
      static_cast<std::int64_t>(proposed.bottom) - proposed.top;
  if (source.width == 0 || source.height == 0 || proposed_width <= 0 ||
      proposed_height <= 0) {
    return proposed;
  }

  auto result = proposed;
  const auto horizontal_frame =
      (std::max)(static_cast<std::int64_t>(insets.left) + insets.right,
                 std::int64_t{0});
  const auto vertical_frame =
      (std::max)(static_cast<std::int64_t>(insets.top) + insets.bottom,
                 std::int64_t{0});
  if (is_vertical_edge(edge)) {
    const auto content_height =
        (std::max)(proposed_height - vertical_frame, std::int64_t{1});
    const auto width =
        rounded_ratio(content_height, source.width, source.height) +
        horizontal_frame;
    if (moves_left(edge)) {
      result.left =
          saturate_coordinate(static_cast<std::int64_t>(result.right) - width);
    } else {
      result.right =
          saturate_coordinate(static_cast<std::int64_t>(result.left) + width);
    }
    return result;
  }

  const auto content_width =
      (std::max)(proposed_width - horizontal_frame, std::int64_t{1});
  const auto height =
      rounded_ratio(content_width, source.height, source.width) +
      vertical_frame;
  if (moves_top(edge)) {
    result.top =
        saturate_coordinate(static_cast<std::int64_t>(result.bottom) - height);
  } else {
    result.bottom =
        saturate_coordinate(static_cast<std::int64_t>(result.top) + height);
  }
  return result;
}

NativeRect content_to_outer_rect(const NativeRect content,
                                 const FrameInsets insets) noexcept {
  return {
      .left = saturate_coordinate(static_cast<std::int64_t>(content.left) -
                                  insets.left),
      .top = saturate_coordinate(static_cast<std::int64_t>(content.top) -
                                 insets.top),
      .right = saturate_coordinate(static_cast<std::int64_t>(content.right) +
                                   insets.right),
      .bottom = saturate_coordinate(static_cast<std::int64_t>(content.bottom) +
                                    insets.bottom),
  };
}

NativeRect outer_to_content_rect(const NativeRect outer,
                                 const FrameInsets insets) noexcept {
  return {
      .left = saturate_coordinate(static_cast<std::int64_t>(outer.left) +
                                  insets.left),
      .top = saturate_coordinate(static_cast<std::int64_t>(outer.top) +
                                 insets.top),
      .right = saturate_coordinate(static_cast<std::int64_t>(outer.right) -
                                   insets.right),
      .bottom = saturate_coordinate(static_cast<std::int64_t>(outer.bottom) -
                                    insets.bottom),
  };
}

NativeRect clamp_outer_rect_to_work_area(const NativeRect outer,
                                         const NativeRect work_area) noexcept {
  const auto horizontal =
      clamp_axis(outer.left, outer.right, work_area.left, work_area.right);
  const auto vertical =
      clamp_axis(outer.top, outer.bottom, work_area.top, work_area.bottom);
  return {.left = horizontal.first,
          .top = vertical.first,
          .right = horizontal.last,
          .bottom = vertical.last};
}

} // namespace airelinna::platform::windows::detail
