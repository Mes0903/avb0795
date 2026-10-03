/// @file
/// @brief Historical SPA layout conversion; native queue authority stays here.
#ifndef AVB0795_LINUX_PIPEWIRE_BUFFER_POLICY_HPP
#define AVB0795_LINUX_PIPEWIRE_BUFFER_POLICY_HPP
#include "platform/linux/detail/cpu_frame_pool.hpp"
#include <functional>
struct spa_buffer;
namespace avb::linux_platform::detail {
  /// @brief Validated source plan into the data-memory base, never the FD origin.
  /// EMPTY plans do not borrow pixels. Ordinary plans borrow the mapping through
  /// copy_cpu_buffer only; wraparound payloads and unsupported memory are rejected.
  struct CpuBufferPlan {
    std::span<const std::byte> mapping;
    core::Extent content{}, allocation{};
    std::size_t source_offset{}, source_pitch{};
    CpuPixelFormat pixels{};
    bool neutral{};
  };
  /// @brief Validate one borrowed native envelope against negotiated CPU format.
  /// mapping starts at the already-adjusted data-memory origin, not the FD's map
  /// offset. Keep envelope/metadata readable during inspection and keep an ordinary
  /// returned plan's pixels stable through copy; no envelope references are retained.
  /// Missing crop or a complete zero-dimension sentinel means full extent. Damaged
  /// metadata/corruption/GAP reject; legal EMPTY returns neutral without reading pixels.
  /// Returns nullopt for unsupported or unsafe layout and never writes source bytes.
  [[nodiscard]] std::optional<CpuBufferPlan> inspect_cpu_buffer(
    const spa_buffer &, CpuFormat, std::span<const std::byte>) noexcept;
  /// @brief Copy validated content to destination origin using its supplied pitch.
  /// Both disjoint spans stay live/stable through this synchronous worker call.
  /// BGRx and neutral content become opaque BGRA; pixels outside content are untouched.
  /// All range/arithmetic checks precede writes: false preserves the destination.
  /// No mapping, native requeue, publication or allocation is performed here.
  [[nodiscard]] bool copy_cpu_buffer(const CpuBufferPlan &, std::span<std::byte>, std::size_t) noexcept;
  /// @brief Convert one validated SPA envelope into the native-independent pool.
  /// The caller keeps metadata/mapping stable through synchronous inspection/copy,
  /// and supplies nonthrowing, nonrecursive native queue authority borrowed only for
  /// this call. Requeue is invoked exactly once on every path, outside the facts
  /// lock and before commit. Failed requeue closes publication; the writer returns
  /// its reserved slot on failure. The same expected revision is used for reserve
  /// and commit, so reconfiguration or Stop during native work cannot publish it.
  /// This transitional adapter owns no storage and manufactures no native envelope.
  [[nodiscard]] bool ingest_cpu_buffer(CpuFramePool &, const spa_buffer &, std::span<const std::byte>, std::chrono::steady_clock::time_point, const std::function<int()> &requeue) noexcept;
}    // namespace avb::linux_platform::detail
#endif
