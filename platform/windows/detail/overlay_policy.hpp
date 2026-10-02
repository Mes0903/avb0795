/// @file
/// @brief Value-only Win32 hit-test, aspect, DPI, and work-area geometry policies.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_OVERLAY_POLICY_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_OVERLAY_POLICY_HPP

#include "avb/core/types.hpp"

#include <cstdint>
#include <optional>

namespace avb::platform::windows::detail {

  inline constexpr std::uintptr_t class_style_own_dc_bit{0x0020};
  inline constexpr std::uintptr_t class_style_class_dc_bit{0x0040};
  inline constexpr std::uintptr_t transparent_style_bit{0x00000020};
  inline constexpr std::uintptr_t layered_style_bit{0x00080000};
  inline constexpr std::uint32_t layered_alpha_flag{0x00000002};

  /// Physical screen-coordinate point used by native hit-test calculations.
  struct NativePoint final {
    std::int32_t x{};
    std::int32_t y{};

    friend bool operator==(const NativePoint &, const NativePoint &) = default;
  };

  /// Signed native rectangle; calculations widen before subtracting coordinates.
  struct NativeRect final {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};

    friend bool operator==(const NativeRect &, const NativeRect &) = default;
  };

  /// Difference between outer window and content edges for the current DPI/style.
  struct FrameInsets final {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};

    friend bool operator==(const FrameInsets &, const FrameInsets &) = default;
  };

  /// Value-only result mapped to Win32 HT codes by the concrete adapter.
  enum class HitZone {
    Delegate,
    Transparent,
    Caption,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
  };

  /// User-dragged edge controlling the anchor of aspect-preserving resizing.
  enum class SizingEdge {
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
  };

  /// Reject CS_OWNDC/CS_CLASSDC because they are incompatible with layered input policy.
  [[nodiscard]] constexpr bool
  is_overlay_class_style_supported(const std::uintptr_t style) noexcept
  {
    return (style & (class_style_own_dc_bit | class_style_class_dc_bit)) == 0;
  }

  /// Test whether a snapshot must include restorable layered attributes.
  [[nodiscard]] constexpr bool
  has_layered_style(const std::uintptr_t style) noexcept
  {
    return (style & layered_style_bit) != 0;
  }

  /// Derive only avb0795-owned transparency bits, preserving unrelated style bits.
  [[nodiscard]] constexpr std::uintptr_t
  input_transparency_style(const std::uintptr_t style,
                           const core::OverlayMode mode) noexcept
  {
    constexpr auto owned_bits = layered_style_bit | transparent_style_bit;
    return mode == core::OverlayMode::PassThrough ? style | owned_bits
                                                  : style & ~owned_bits;
  }

  /// Keep OS chrome delegated; borderless Editable uses resize bands and caption drag.
  [[nodiscard]] HitZone overlay_hit_test(core::OverlayMode mode,
                                         core::BorderPreference border,
                                         NativeRect window,
                                         NativePoint cursor,
                                         std::uint32_t resize_band) noexcept;

  /// Convert the logical resize band to physical pixels using the active monitor DPI.
  [[nodiscard]] std::uint32_t scaled_resize_band(std::uint32_t logical_pixels,
                                                 std::uint32_t dpi) noexcept;

  /// @brief Constrain client-content aspect while preserving the opposite drag anchor.
  ///
  /// Current DPI-dependent frame insets are excluded from the aspect calculation.
  /// Top and bottom drags take height as authoritative; other edges use width.
  /// Invalid extents return the proposal unchanged, and native-coordinate
  /// saturation can limit exact aspect preservation at representational bounds.
  [[nodiscard]] NativeRect
  aspect_constrained_rect(NativeRect proposed, SizingEdge edge, core::Extent source, FrameInsets insets = {}) noexcept;

  /// Add current border insets without changing the intended content rectangle.
  [[nodiscard]] NativeRect content_to_outer_rect(NativeRect content,
                                                 FrameInsets insets) noexcept;

  /// Remove current border insets before applying content-aspect calculations.
  [[nodiscard]] NativeRect outer_to_content_rect(NativeRect outer,
                                                 FrameInsets insets) noexcept;

  /// @brief Fit each outer-rectangle axis into the selected monitor work area.
  ///
  /// Placement is shifted when its size fits; an oversized axis is reduced to the
  /// available span. This policy can change size and aspect, so it is a placement
  /// bound rather than a promise to restore the original client geometry exactly.
  [[nodiscard]] NativeRect
  clamp_outer_rect_to_work_area(NativeRect outer, NativeRect work_area) noexcept;

  /// @brief Fit client width to content aspect, returning the bounded outer rectangle.
  ///
  /// Preserve the client center, then shift within the work area. Oversized content
  /// shrinks uniformly after subtracting frame insets; rounded axes stay at least
  /// one pixel. Invalid or unrepresentable inputs return no proposal.
  [[nodiscard]] std::optional<NativeRect> fit_content_rect(
    NativeRect client_screen, core::Extent source, FrameInsets insets,
    NativeRect work_area) noexcept;

  /// @brief Resize around the client center, preserving the effective source aspect.
  /// The DPI-scaled minimum yields to work-area capacity; invalid native inputs
  /// return no proposal. The result includes frame insets and fits SetWindowPos.
  [[nodiscard]] std::optional<NativeRect> wheel_resized_rect(
    NativeRect client_screen, core::Extent source, FrameInsets insets,
    NativeRect work_area, std::uint32_t dpi, int notches, int step_percent) noexcept;

}    // namespace avb::platform::windows::detail

#endif    // AVB0795_PLATFORM_WINDOWS_DETAIL_OVERLAY_POLICY_HPP
