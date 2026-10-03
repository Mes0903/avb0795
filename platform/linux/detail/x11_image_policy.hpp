/// @file
/// @brief Checked, native-independent conversion of rectangle-sized X images.
#ifndef AVB0795_LINUX_X11_IMAGE_POLICY_HPP
#define AVB0795_LINUX_X11_IMAGE_POLICY_HPP
#include "avb/core/types.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
namespace avb::linux_platform::detail {
  enum class X11ByteOrder { LeastSignificant, MostSignificant };
  /// @brief Describe drawable bounds separately from the returned rectangle bytes.
  /// Setup supplies pixel storage and visual masks; the native owner supplies the
  /// checked drawable geometry and exact protocol stride. No native owner is held.
  struct X11ImageFacts {
    core::Extent allocation{}, drawable_extent{};
    core::PixelRegion region{};
    std::size_t stride{};
    std::uint8_t depth{}, bits_per_pixel{}, scanline_pad{};
    X11ByteOrder byte_order{};
    std::uint32_t red_mask{}, green_mask{}, blue_mask{};
    bool true_color{};
  };
  /// @brief Borrow a validated image only until synchronous copy finishes.
  /// The caller retains immutable payload storage; neither operation allocates or
  /// retains native resources. Mutable scalar plans are revalidated by copy, so
  /// malformed destinations or changed plans fail before any destination write.
  struct X11ImagePlan { X11ImageFacts facts; std::span<const std::byte> payload; };
  [[nodiscard]] std::optional<X11ImagePlan> inspect_x11_image(const X11ImageFacts &, std::span<const std::byte>) noexcept;
  /// @brief Normalize disjoint source/destination storage to opaque BGRA bytes.
  /// Region coordinates validate drawable containment, never offset the already
  /// rectangle-sized payload. All row spans and overlap are checked before copy;
  /// the caller exclusively owns destination bytes through return.
  [[nodiscard]] bool copy_x11_image(const X11ImagePlan &, std::span<std::byte>, std::size_t destination_pitch) noexcept;
}
#endif
