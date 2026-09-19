/// @file
/// @brief Validate a complete BGRA8 layout before packing content and padding.
#include "render/cpu_row_copy.hpp"

#include <cstring>
#include <limits>

namespace avb::render {

  std::expected<RowCopyStats, RowCopyError>
  copy_bgra_rows(const CpuRowsView source, const std::span<std::byte> destination) noexcept
  {
    if (source.content.width == 0 || source.content.height == 0 ||
        source.content.width > source.allocation.width ||
        source.content.height > source.allocation.height) {
      return std::unexpected{RowCopyError::InvalidExtent};
    }

    constexpr std::size_t bytes_per_pixel = 4;
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    const auto width = static_cast<std::size_t>(source.allocation.width);
    const auto height = static_cast<std::size_t>(source.allocation.height);
    if (width > maximum / bytes_per_pixel) {
      return std::unexpected{RowCopyError::Overflow};
    }
    const auto destination_pitch = width * bytes_per_pixel;
    if (height > maximum / destination_pitch) {
      return std::unexpected{RowCopyError::Overflow};
    }
    const auto allocation_bytes = destination_pitch * height;
    // Containment bounds every content product by the validated allocation.
    const auto row_bytes = static_cast<std::size_t>(source.content.width) * bytes_per_pixel;
    const auto content_height = static_cast<std::size_t>(source.content.height);
    if (source.row_pitch < row_bytes) {
      return std::unexpected{RowCopyError::InvalidPitch};
    }
    const auto last_row = content_height - 1;
    if (last_row != 0 && source.row_pitch > (maximum - row_bytes) / last_row) {
      return std::unexpected{RowCopyError::Overflow};
    }
    const auto source_bytes = last_row * source.row_pitch + row_bytes;
    if (source.bytes.size() < source_bytes || destination.size() != allocation_bytes) {
      return std::unexpected{RowCopyError::InvalidSpan};
    }

    const auto right_padding = destination_pitch - row_bytes;
    for (std::size_t row{}; row < content_height; ++row) {
      auto *const output = destination.data() + row * destination_pitch;
      std::memcpy(output, source.bytes.data() + row * source.row_pitch, row_bytes);
      if (right_padding != 0) {
        std::memset(output + row_bytes, 0, right_padding);
      }
    }
    const auto lower_offset = content_height * destination_pitch;
    if (lower_offset < allocation_bytes) {
      std::memset(destination.data() + lower_offset, 0, allocation_bytes - lower_offset);
    }
    const auto copied_bytes = content_height * row_bytes;
    return RowCopyStats{copied_bytes, allocation_bytes - copied_bytes};
  }

}    // namespace avb::render
