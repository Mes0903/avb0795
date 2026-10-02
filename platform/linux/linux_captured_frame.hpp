/// @file
/// @brief Move-only CPU frame leases independent of presentation and native buffers.
#ifndef AVB0795_LINUX_CAPTURED_FRAME_HPP
#define AVB0795_LINUX_CAPTURED_FRAME_HPP
#include "avb/core/types.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
namespace avb::linux_platform {
namespace detail { struct CpuPoolState; class CpuFramePool; }
/// @brief Own one immutable pool slot until synchronous readers finish.
/// A frame retains its three-slot pool after capture retirement. Moving transfers
/// the lease; destroying it returns the slot. No native buffer is borrowed.
class LinuxCapturedFrame final {
public:
  LinuxCapturedFrame() = default;
  ~LinuxCapturedFrame();
  LinuxCapturedFrame(const LinuxCapturedFrame &) = delete;
  LinuxCapturedFrame &operator=(const LinuxCapturedFrame &) = delete;
  LinuxCapturedFrame(LinuxCapturedFrame &&) noexcept;
  LinuxCapturedFrame &operator=(LinuxCapturedFrame &&) noexcept;
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
  [[nodiscard]] std::size_t row_pitch() const noexcept { return pitch_; }
  [[nodiscard]] core::Extent content_extent() const noexcept { return content_; }
  [[nodiscard]] core::Extent allocation_extent() const noexcept { return allocation_; }
  [[nodiscard]] std::uint64_t allocation_revision() const noexcept { return revision_; }
  [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
  [[nodiscard]] std::chrono::steady_clock::time_point intake_observed_at() const noexcept { return observed_; }
private:
  LinuxCapturedFrame(std::shared_ptr<detail::CpuPoolState>, std::size_t,
    std::size_t, core::Extent, core::Extent, std::uint64_t, std::uint64_t,
    std::chrono::steady_clock::time_point) noexcept;
  void release() noexcept;
  std::shared_ptr<detail::CpuPoolState> pool_;
  std::size_t slot_{}, pitch_{};
  core::Extent content_{}, allocation_{};
  std::uint64_t revision_{}, sequence_{};
  std::chrono::steady_clock::time_point observed_{};
  friend class detail::CpuFramePool;
};
}
#endif
