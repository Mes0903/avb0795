/// @file
/// @brief Owning controller font payload shared by platform composition and presentation.
#ifndef AVB0795_DESKTOP_CONTROLLER_FONT_DATA_HPP
#define AVB0795_DESKTOP_CONTROLLER_FONT_DATA_HPP

#include <cstddef>
#include <vector>

namespace avb::desktop {
  /// @brief Retain a complete font file and its selected collection face.
  /// Move this value into the host before atlas registration. The atlas borrows
  /// the unchanged bytes until context teardown; face_index must be nonnegative.
  /// A collection's face identity is part of the payload, not inferred from bytes.
  struct ControllerFontData final {
    std::vector<std::byte> bytes;
    int face_index{};
  };
}    // namespace avb::desktop

#endif
