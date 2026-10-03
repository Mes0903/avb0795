/// @file
/// @brief Return a single immutable slot lease without borrowing the capture owner.
#include "platform/linux/linux_captured_frame.hpp"
#include "platform/linux/detail/cpu_frame_pool.hpp"
#include <utility>
namespace avb::linux_platform {
LinuxCapturedFrame::LinuxCapturedFrame(std::shared_ptr<detail::CpuPoolState> pool,
  std::size_t slot,std::size_t pitch,core::Extent content,core::Extent allocation,
  std::uint64_t revision,std::uint64_t sequence,std::chrono::steady_clock::time_point observed) noexcept
  : pool_{std::move(pool)},slot_{slot},pitch_{pitch},content_{content},allocation_{allocation},
    revision_{revision},sequence_{sequence},observed_{observed} {}
LinuxCapturedFrame::~LinuxCapturedFrame() { release(); }
LinuxCapturedFrame::LinuxCapturedFrame(LinuxCapturedFrame &&other) noexcept
  : pool_{std::move(other.pool_)},slot_{other.slot_},pitch_{other.pitch_},content_{other.content_},
    allocation_{other.allocation_},revision_{other.revision_},sequence_{other.sequence_},observed_{other.observed_} {}
LinuxCapturedFrame &LinuxCapturedFrame::operator=(LinuxCapturedFrame &&other) noexcept {
  if (this!=&other) {
    release(); pool_=std::move(other.pool_); slot_=other.slot_; pitch_=other.pitch_;
    content_=other.content_; allocation_=other.allocation_; revision_=other.revision_;
    sequence_=other.sequence_; observed_=other.observed_;
  }
  return *this;
}
void LinuxCapturedFrame::release() noexcept {
  if (!pool_) return;
  { std::lock_guard lock{pool_->mutex}; pool_->slots[slot_].leased=false; ++pool_->facts.epoch; }
  pool_.reset();
}
std::span<const std::byte> LinuxCapturedFrame::bytes() const noexcept {
  return pool_ ? std::span<const std::byte>{pool_->slots[slot_].bytes} : std::span<const std::byte>{};
}
}
