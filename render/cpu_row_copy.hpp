/// @file
/// @brief Synchronous BGRA8 row packing and byte accounting for CPU uploads.
#ifndef AVB0795_RENDER_CPU_ROW_COPY_HPP
#define AVB0795_RENDER_CPU_ROW_COPY_HPP

#include "render/vulkan_input.hpp"

#include <cstddef>
#include <expected>
#include <span>

namespace avb::render {

  /// @brief Bytes written by a completed pack, separated by their source.
  /// copied_bytes counts content pixels; cleared_bytes counts allocation padding.
  /// Their sum equals the tight destination allocation, allowing the caller to
  /// report CPU memory traffic independently of the subsequent Vulkan upload.
  struct RowCopyStats {
    std::size_t copied_bytes{};
    std::size_t cleared_bytes{};
  };

  /// @brief Invalid layout category detected before the first destination write.
  /// Extent checks establish positive, contained content. Pitch checks establish
  /// a complete source row; span checks establish readable/writable byte ranges.
  /// Overflow reports arithmetic that exceeds host-addressable size_t storage.
  enum class RowCopyError { InvalidExtent, InvalidPitch, InvalidSpan, Overflow };

  /// @brief Pack captured BGRA8 pixels and zero the allocation's remaining pixels.
  ///
  /// A full-allocation upload needs defined bytes outside the current content,
  /// including pixels exposed when a later frame shrinks. Each content row is
  /// copied using the producer's pitch; its right-hand padding and the lower
  /// allocation rows are cleared. The destination uses allocation.width * 4
  /// bytes per row and its span size must equal allocation.width * height * 4.
  /// The source span needs (content.height - 1) * row_pitch + content.width * 4
  /// bytes, so trailing padding after the final content row is optional.
  ///
  /// @pre Both spans refer to live, disjoint storage throughout this synchronous
  /// call. The producer keeps source rows mapped and the consumer owns destination
  /// write access. The caller retains mapping, flushing and upload responsibilities.
  /// @return Successful write counts, or a layout error preserving every destination
  /// byte. Extent, arithmetic, pitch and span validation precede all memory access.
  [[nodiscard]] std::expected<RowCopyStats, RowCopyError>
  copy_bgra_rows(CpuRowsView source, std::span<std::byte> destination) noexcept;

}    // namespace avb::render

#endif    // AVB0795_RENDER_CPU_ROW_COPY_HPP
