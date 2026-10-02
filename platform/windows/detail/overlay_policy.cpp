/// @file
/// @brief Value-only Win32 hit-test, aspect, DPI, and work-area geometry policies.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/detail/overlay_policy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace avb::platform::windows::detail {
  namespace {

    [[nodiscard]] std::int64_t
    rounded_ratio(const std::int64_t value, const std::uint32_t numerator, const std::uint32_t denominator) noexcept
    {
      if (value <= 0 || numerator == 0 || denominator == 0) {
        return value;
      }
      // Widen multiplication and round before narrowing to native coordinate limits.
      const auto scaled =
        static_cast<std::uint64_t>(value) * numerator + denominator / 2U;
      return static_cast<std::int64_t>(
        (std::min) (scaled / denominator,
                    static_cast<std::uint64_t>(
                      (std::numeric_limits<std::int32_t>::max)())));
    }

    [[nodiscard]] constexpr std::int32_t
    saturate_coordinate(const std::int64_t value) noexcept
    {
      return static_cast<std::int32_t>(
        (std::clamp) (value,
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
                                       const std::int32_t work_last) noexcept
    {
      const auto available = static_cast<std::int64_t>(work_last) - work_first;
      if (available <= 0) {
        return {.first = work_first, .last = work_first};
      }
      const auto requested =
        (std::max) (static_cast<std::int64_t>(last) - first, std::int64_t{0});
      if (requested >= available) {
        return {.first = work_first, .last = work_last};
      }
      const auto latest_first = static_cast<std::int64_t>(work_last) - requested;
      const auto clamped_first =
        (std::clamp) (static_cast<std::int64_t>(first),
                      static_cast<std::int64_t>(work_first), latest_first);
      return {.first = saturate_coordinate(clamped_first),
              .last = saturate_coordinate(clamped_first + requested)};
    }

    [[nodiscard]] bool moves_left(const SizingEdge edge) noexcept
    {
      return edge == SizingEdge::Left || edge == SizingEdge::TopLeft ||
             edge == SizingEdge::BottomLeft;
    }

    [[nodiscard]] bool moves_top(const SizingEdge edge) noexcept
    {
      return edge == SizingEdge::Top || edge == SizingEdge::TopLeft ||
             edge == SizingEdge::TopRight;
    }

    [[nodiscard]] bool is_vertical_edge(const SizingEdge edge) noexcept
    {
      return edge == SizingEdge::Top || edge == SizingEdge::Bottom;
    }

  }    // namespace

  HitZone overlay_hit_test(const core::OverlayMode mode,
                           const core::BorderPreference border,
                           const NativeRect window,
                           const NativePoint cursor,
                           const std::uint32_t resize_band) noexcept
  {
    if (mode == core::OverlayMode::PassThrough) {
      return HitZone::Transparent;
    }
    // Native chrome keeps its standard hit testing; custom bands serve borderless mode.
    if (border == core::BorderPreference::Bordered) {
      return HitZone::Delegate;
    }

    const auto width =
      (std::max) (static_cast<std::int64_t>(window.right) - window.left,
                  std::int64_t{0});
    const auto height =
      (std::max) (static_cast<std::int64_t>(window.bottom) - window.top,
                  std::int64_t{0});
    // Small windows cannot have overlapping resize bands wider than half either dimension.
    const auto band = (std::min) ({width / 2, height / 2,
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
                                   const std::uint32_t dpi) noexcept
  {
    constexpr std::uint64_t default_dpi{96};
    const auto scaled =
      static_cast<std::uint64_t>(logical_pixels) * dpi + default_dpi / 2U;
    return static_cast<std::uint32_t>(
      (std::min) (scaled / default_dpi,
                  static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)())));
  }

  NativeRect aspect_constrained_rect(const NativeRect proposed,
                                     const SizingEdge edge,
                                     const core::Extent source,
                                     const FrameInsets insets) noexcept
  {
    const auto proposed_width =
      static_cast<std::int64_t>(proposed.right) - proposed.left;
    const auto proposed_height =
      static_cast<std::int64_t>(proposed.bottom) - proposed.top;
    if (source.width == 0 || source.height == 0 || proposed_width <= 0 ||
        proposed_height <= 0) {
      return proposed;
    }

    // Apply aspect to content, not the outer rectangle containing DPI-dependent borders.
    auto result = proposed;
    const auto horizontal_frame =
      (std::max) (static_cast<std::int64_t>(insets.left) + insets.right,
                  std::int64_t{0});
    const auto vertical_frame =
      (std::max) (static_cast<std::int64_t>(insets.top) + insets.bottom,
                  std::int64_t{0});
    // Choose the user's dragged dimension as authoritative and preserve the opposite anchor.
    if (is_vertical_edge(edge)) {
      const auto content_height =
        (std::max) (proposed_height - vertical_frame, std::int64_t{1});
      const auto width =
        rounded_ratio(content_height, source.width, source.height) +
        horizontal_frame;
      if (moves_left(edge)) {
        result.left =
          saturate_coordinate(static_cast<std::int64_t>(result.right) - width);
      }
      else {
        result.right =
          saturate_coordinate(static_cast<std::int64_t>(result.left) + width);
      }
      return result;
    }

    const auto content_width =
      (std::max) (proposed_width - horizontal_frame, std::int64_t{1});
    const auto height =
      rounded_ratio(content_width, source.height, source.width) +
      vertical_frame;
    if (moves_top(edge)) {
      result.top =
        saturate_coordinate(static_cast<std::int64_t>(result.bottom) - height);
    }
    else {
      result.bottom =
        saturate_coordinate(static_cast<std::int64_t>(result.top) + height);
    }
    return result;
  }

  NativeRect content_to_outer_rect(const NativeRect content,
                                   const FrameInsets insets) noexcept
  {
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
                                   const FrameInsets insets) noexcept
  {
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
                                           const NativeRect work_area) noexcept
  {
    const auto horizontal =
      clamp_axis(outer.left, outer.right, work_area.left, work_area.right);
    const auto vertical =
      clamp_axis(outer.top, outer.bottom, work_area.top, work_area.bottom);
    return {.left = horizontal.first,
            .top = vertical.first,
            .right = horizontal.last,
            .bottom = vertical.last};
  }

  namespace {

    [[nodiscard]] std::optional<NativeRect> bounded_content_rect(
      const NativeRect client_screen, const core::Extent source, const FrameInsets insets,
      const NativeRect work_area, const double factor,
      const double minimum_width, const double minimum_height) noexcept
    {
      constexpr auto maximum_dimension = (std::numeric_limits<int>::max)();
      const auto client_width = static_cast<std::int64_t>(client_screen.right) - client_screen.left;
      const auto client_height = static_cast<std::int64_t>(client_screen.bottom) - client_screen.top;
      const auto work_width = static_cast<std::int64_t>(work_area.right) - work_area.left;
      const auto work_height = static_cast<std::int64_t>(work_area.bottom) - work_area.top;
      const auto horizontal_frame = static_cast<std::int64_t>(insets.left) + insets.right;
      const auto vertical_frame = static_cast<std::int64_t>(insets.top) + insets.bottom;
      if (source.width == 0 || source.height == 0 ||
          insets.left < 0 || insets.top < 0 || insets.right < 0 || insets.bottom < 0 ||
          client_width <= 0 || client_height <= 0 || work_width <= 0 || work_height <= 0 ||
          client_width + horizontal_frame > maximum_dimension ||
          client_height + vertical_frame > maximum_dimension ||
          work_width > maximum_dimension || work_height > maximum_dimension ||
          horizontal_frame >= work_width || vertical_frame >= work_height) {
        return std::nullopt;
      }

      const auto available_width = work_width - horizontal_frame;
      const auto available_height = work_height - vertical_frame;
      const double ratio = static_cast<double>(source.width) / source.height;
      const double upper_width = (std::min)(static_cast<double>(available_width),
                                           available_height * ratio);
      const double lower_width = (std::min)(upper_width,
        (std::max)(minimum_width, minimum_height * ratio));
      // Positive overflow and negative underflow have the same bounded direction
      // as a finite scale. Clamp before converting any floating-point dimensions.
      const double width = std::isfinite(factor)
        ? (std::clamp)(client_width * factor, lower_width, upper_width)
        : upper_width;
      const auto rounded_width = static_cast<std::int64_t>((std::clamp)(
        std::round(width), 1.0, static_cast<double>(available_width)));
      const auto rounded_height = static_cast<std::int64_t>((std::clamp)(
        std::round(width / ratio), 1.0, static_cast<double>(available_height)));
      const auto outer_width = rounded_width + horizontal_frame;
      const auto outer_height = rounded_height + vertical_frame;

      // Widen both the center and placement arithmetic: valid screen coordinates
      // can straddle zero or sit at either signed native-coordinate limit.
      const double center_x = (static_cast<std::int64_t>(client_screen.left) + client_screen.right) / 2.0;
      const double center_y = (static_cast<std::int64_t>(client_screen.top) + client_screen.bottom) / 2.0;
      const auto left = (std::clamp)(
        static_cast<std::int64_t>(std::llround(center_x - rounded_width / 2.0 - insets.left)),
        static_cast<std::int64_t>(work_area.left),
        static_cast<std::int64_t>(work_area.right) - outer_width);
      const auto top = (std::clamp)(
        static_cast<std::int64_t>(std::llround(center_y - rounded_height / 2.0 - insets.top)),
        static_cast<std::int64_t>(work_area.top),
        static_cast<std::int64_t>(work_area.bottom) - outer_height);
      return NativeRect{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                        static_cast<std::int32_t>(left + outer_width),
                        static_cast<std::int32_t>(top + outer_height)};
    }

  }    // namespace

  std::optional<NativeRect> fit_content_rect(
    const NativeRect client_screen, const core::Extent source, const FrameInsets insets,
    const NativeRect work_area) noexcept
  {
    return bounded_content_rect(client_screen, source, insets, work_area, 1.0, 0.0, 0.0);
  }

  std::optional<NativeRect> wheel_resized_rect(
    const NativeRect client_screen, const core::Extent source, const FrameInsets insets,
    const NativeRect work_area, const std::uint32_t dpi, const int notches,
    const int step_percent) noexcept
  {
    if (dpi == 0 || !core::valid_wheel_zoom_percent(step_percent)) {
      return std::nullopt;
    }
    constexpr double default_dpi{96.0};
    return bounded_content_rect(client_screen, source, insets, work_area,
      std::pow(1.0 + step_percent / 100.0, notches),
      160.0 * dpi / default_dpi, 90.0 * dpi / default_dpi);
  }

}    // namespace avb::platform::windows::detail
