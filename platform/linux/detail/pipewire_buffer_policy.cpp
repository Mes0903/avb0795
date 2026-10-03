/// @file
/// @brief Historical SPA inspection/conversion and exactly-once native requeue.
#include "platform/linux/detail/pipewire_buffer_policy.hpp"
#include <spa/buffer/meta.h>
#include <spa/buffer/buffer.h>
#include <cstring>
#include <limits>
namespace avb::linux_platform::detail {
  namespace {
    bool add(std::size_t a, std::size_t b, std::size_t &out) noexcept
    {
      if (b > std::numeric_limits<std::size_t>::max() - a) return false;
      out = a + b;
      return true;
    }
    bool mul(std::size_t a, std::size_t b, std::size_t &out) noexcept
    {
      if (a && b > std::numeric_limits<std::size_t>::max() / a) return false;
      out = a * b;
      return true;
    }
  }    // namespace
  std::optional<CpuBufferPlan> inspect_cpu_buffer(const spa_buffer &buffer,
                                                  CpuFormat format,
                                                  std::span<const std::byte> mapping) noexcept
  {
    const auto a = format.allocation;
    if (!a.width || !a.height || buffer.n_datas != 1 || !buffer.datas ||
        (buffer.n_metas && !buffer.metas)) return {};
    const auto &data = buffer.datas[0];
    if ((data.type != SPA_DATA_MemPtr && data.type != SPA_DATA_MemFd) ||
        !(data.flags & SPA_DATA_FLAG_READABLE) || !data.chunk) return {};
    const auto &chunk = *data.chunk;
    if (chunk.flags & SPA_CHUNK_FLAG_CORRUPTED) return {};
    core::Extent content = a;
    std::size_t x = 0, y = 0;
    bool header_seen = false, crop_seen = false;
    for (std::uint32_t i = 0; i < buffer.n_metas; ++i) {
      const auto &meta = buffer.metas[i];
      if (meta.type == SPA_META_Header) {
        if (header_seen || !meta.data || meta.size < sizeof(spa_meta_header)) return {};
        header_seen = true;
        spa_meta_header header{};
        std::memcpy(&header, meta.data, sizeof header);
        if (header.flags & (SPA_META_HEADER_FLAG_CORRUPTED | SPA_META_HEADER_FLAG_GAP)) return {};
      }
      else if (meta.type == SPA_META_VideoCrop) {
        if (crop_seen || !meta.data || meta.size < sizeof(spa_meta_region)) return {};
        crop_seen = true;
        spa_meta_region crop{};
        std::memcpy(&crop, meta.data, sizeof crop);
        const auto &r = crop.region;
        // SPA marks a complete region with either zero dimension as no crop.
        // Envelope damage is checked above and cannot masquerade as that sentinel.
        if (!r.size.width || !r.size.height) continue;
        if (r.position.x < 0 || r.position.y < 0) return {};
        x = static_cast<std::size_t>(r.position.x);
        y = static_cast<std::size_t>(r.position.y);
        if (x >= a.width || y >= a.height || r.size.width > a.width - x || r.size.height > a.height - y) return {};
        content = {r.size.width, r.size.height};
      }
    }
    CpuBufferPlan result{mapping, content, a, 0, 0, format.pixels, (chunk.flags & SPA_CHUNK_FLAG_EMPTY) != 0};
    if (result.neutral) {
      result.mapping = {};
      return result;
    }
    std::size_t full_row{}, xbytes{}, ystart{}, start{}, tail{}, row{}, end{}, absolute{};
    if (!chunk.size || chunk.stride <= 0 || !mul(a.width, 4, full_row) ||
        static_cast<std::size_t>(chunk.stride) < full_row ||
        chunk.offset > data.maxsize || chunk.size > data.maxsize - chunk.offset ||
        !mul(x, 4, xbytes) || !mul(y, chunk.stride, ystart) || !add(ystart, xbytes, start) ||
        !mul(content.height - 1, chunk.stride, tail) || !mul(content.width, 4, row) ||
        !add(start, tail, end) || !add(end, row, end) || end > chunk.size ||
        !add(chunk.offset, end, absolute) || absolute > mapping.size()) return {};
    result.source_offset = chunk.offset + start;
    result.source_pitch = chunk.stride;
    return result;
  }
  bool copy_cpu_buffer(const CpuBufferPlan &p, std::span<std::byte> destination, std::size_t pitch) noexcept
  {
    std::size_t row{}, tail{}, required{};
    if (!p.content.width || !p.content.height || p.content.width > p.allocation.width ||
        p.content.height > p.allocation.height || !mul(p.content.width, 4, row) || pitch < row ||
        !mul(p.content.height - 1, pitch, tail) || !add(tail, row, required) || required > destination.size()) return false;
    if (!p.neutral) {
      if (p.source_pitch < row || !mul(p.content.height - 1, p.source_pitch, tail) ||
          !add(tail, row, required) || !add(p.source_offset, required, required) || required > p.mapping.size()) return false;
    }
    for (std::size_t y = 0; y < p.content.height; ++y) {
      auto *out = destination.data() + y * pitch;
      if (p.neutral)
        std::memset(out, 0, row);
      else
        std::memcpy(out, p.mapping.data() + p.source_offset + y * p.source_pitch, row);
      if (p.neutral || p.pixels == CpuPixelFormat::Bgrx)
        for (std::size_t x = 3; x < row; x += 4) out[x] = std::byte{255};
    }
    return true;
  }
  bool ingest_cpu_buffer(CpuFramePool &pool, const spa_buffer &buffer, std::span<const std::byte> mapping, std::chrono::steady_clock::time_point observed, const std::function<int()> &requeue) noexcept
  {
    pool.service_storage();
    const auto facts = pool.facts();
    const auto plan = facts.format && !facts.stopping && !facts.lost
                        ? inspect_cpu_buffer(buffer, *facts.format, mapping)
                        : std::nullopt;
    auto writer = plan ? pool.begin_write(facts.revision, plan->content, observed) : std::nullopt;
    const bool copied = writer && copy_cpu_buffer(*plan, writer->bytes(), writer->row_pitch());
    // Retire native ownership exactly once even for rejected metadata, full slots,
    // Stop and superseded allocations. The writer remains exclusive through requeue
    // while the main-facing pool mutex remains available. No publication precedes it.
    const int queued = requeue();
    if (queued < 0) {
      pool.fail(core::AppError{core::AppErrorCode::PlatformOperationFailure});
      return false;
    }
    return copied && writer->commit();
  }
}    // namespace avb::linux_platform::detail
