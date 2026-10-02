/// @file
/// @brief Map native scalar facts into the existing renderer input contract.
#include "platform/linux/linux_cpu_frame_bridge.hpp"
#include "render/vulkan_renderer.hpp"
#include <limits>
namespace avb::linux_platform {
bool detail::valid_cpu_delivery(const LinuxCapturedFrame &frame,std::uint64_t revision) noexcept {
  const auto content=frame.content_extent(),allocation=frame.allocation_extent();
  if (!revision || frame.allocation_revision()!=revision || !frame.sequence() ||
      !content.width || !content.height || content.width>allocation.width || content.height>allocation.height ||
      content.width>std::numeric_limits<std::size_t>::max()/4) return false;
  const auto row=static_cast<std::size_t>(content.width)*4,pitch=frame.row_pitch();
  if (pitch<row || (content.height-1)>std::numeric_limits<std::size_t>::max()/pitch) return false;
  const auto tail=static_cast<std::size_t>(content.height-1)*pitch;
  return row<=std::numeric_limits<std::size_t>::max()-tail && tail+row<=frame.bytes().size();
}
std::expected<bool,core::AppError> LinuxCpuFrameBridge::deliver(render::VulkanRenderer &r,LinuxCapturedFrame &frame) noexcept {
  return detail::deliver_cpu_frame(frame,r.generation(),[&] { return r.reserve_source_slot(); },
    [&](auto key,auto rows,auto stamp) { return r.complete_cpu_input(key,rows,stamp); },
    [&](auto key) { return r.discard_completed_source(key); });
}
}
