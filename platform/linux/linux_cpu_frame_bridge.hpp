/// @file
/// @brief Synchronous CPU lease-to-renderer delivery on the presentation thread.
#ifndef AVB0795_LINUX_CPU_FRAME_BRIDGE_HPP
#define AVB0795_LINUX_CPU_FRAME_BRIDGE_HPP
#include "platform/linux/linux_captured_frame.hpp"
#include "render/vulkan_input.hpp"
#include <expected>
namespace avb::render { class VulkanRenderer; }
namespace avb::linux_platform {
namespace detail {
/// @brief Validate a leased frame before any renderer reservation is attempted.
[[nodiscard]] bool valid_cpu_delivery(const LinuxCapturedFrame &,std::uint64_t) noexcept;
/// @brief Narrow synchronous handshake shared by the concrete bridge and policy tests.
/// Callables are borrowed only for this call, with the existing renderer operations'
/// exact semantics. A busy ring retains the frame. Once completion was called,
/// any error discards the adopted source, since copying/flush may have changed it.
template<class Reserve,class Complete,class Discard>
std::expected<bool,core::AppError> deliver_cpu_frame(LinuxCapturedFrame &frame,
  std::uint64_t generation,Reserve &&reserve,Complete &&complete,Discard &&discard) noexcept {
  if (!valid_cpu_delivery(frame,generation)) return std::unexpected(core::AppError{core::AppErrorCode::RuntimeStateLost});
  auto slot=reserve(); if (!slot) return false;
  auto result=complete(*slot,render::CpuRowsView{frame.bytes(),frame.row_pitch(),frame.content_extent(),frame.allocation_extent()},
    render::FrameStamp{frame.sequence(),frame.intake_observed_at()});
  if (!result) {
    auto discharged=discard(*slot);
    return std::unexpected(discharged ? result.error() : discharged.error());
  }
  return true;
}
}
/// @brief Copy a matching current-revision CPU lease into a renderer-owned slot.
/// true permits lease release after synchronous copy; false retains it for latest
/// retry. Typed failure discharges any reservation before returning. Main thread
/// only; the runtime has installed this capture's exact allocation and revision.
class LinuxCpuFrameBridge final {
public:
  [[nodiscard]] static std::expected<bool,core::AppError> deliver(render::VulkanRenderer &,LinuxCapturedFrame &) noexcept;
};
}
#endif
