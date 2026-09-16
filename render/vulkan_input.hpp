/// @file
/// @brief Producer-to-renderer input values; borrowed storage stays with its native owner.
#ifndef AVB0795_RENDER_VULKAN_INPUT_HPP
#define AVB0795_RENDER_VULKAN_INPUT_HPP

#include "avb/core/types.hpp"

#include <vulkan/vulkan_core.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>

namespace avb::render {

  /// @brief Synchronous device preferences consumed only during renderer startup.
  /// The extension span and callable are borrowed for construction; the renderer
  /// stores neither. Optional extensions are enabled as one complete group.
  struct VulkanStartupRequest {
    std::function<bool(VkPhysicalDevice)> prefer_candidate;
    std::span<const char *const> optional_device_extension_group;
    bool request_timeline_semaphore{};
  };

  /// @brief Renderer-owned device borrowed by a platform import owner.
  /// Imported resources and all uses of them must retire and be released before
  /// the renderer destroys this device. No queue or surface ownership is lent.
  /// Enabled facts describe the created device, not just advertised support.
  struct VulkanDeviceBorrow {
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceLimits limits{};
    bool optional_extension_group_enabled{};
    bool timeline_semaphore_enabled{};
  };

  /// @brief Identity of one slot in a particular renderer ring installation.
  ///
  /// The renderer issues a nonzero serial for each installation independently of
  /// the runtime's capture generation. A key is a value, not a reservation lease:
  /// consumers must validate serial, index and the required phase before use.
  struct SlotKey {
    std::uint64_t ring_serial{};
    std::uint32_t index{};
    friend bool operator==(const SlotKey &, const SlotKey &) = default;
  };

  /// @brief Allocate an installation identity independently of capture generation.
  /// Every attempted install consumes its candidate, including rollback/fallback.
  /// Exhaustion returns nullopt so the owner enters terminal recovery before reuse.
  [[nodiscard]] constexpr std::optional<std::uint64_t>
  next_ring_serial(const std::uint64_t last_issued) noexcept
  {
    if (last_issued == (std::numeric_limits<std::uint64_t>::max)()) {
      return std::nullopt;
    }
    return last_issued + 1;
  }

  /// @brief Validate identity and bounds before indexing the owner's live ring.
  /// The caller additionally validates its required canonical phase. Serial zero
  /// denotes a detached ring; a key is usable solely within its installed capacity.
  [[nodiscard]] constexpr bool matches_slot_key(
    const SlotKey key, const std::uint64_t installed_serial,
    const std::size_t capacity) noexcept
  {
    return installed_serial != 0 && key.ring_serial == installed_serial &&
           key.index < capacity;
  }

  /// @brief Source-frame ordering and intake observation carried through a draw.
  ///
  /// The producer supplies its sequence and the steady-clock time at intake.
  /// Retained redraws preserve both values so their source age remains observable;
  /// neither value proves producer completion or reserves a renderer slot.
  struct FrameStamp {
    std::uint64_t sequence{};
    std::chrono::steady_clock::time_point intake_observed_at{};
  };

  /// @brief Borrowed BGRA8 rows consumed synchronously while the producer keeps them mapped.
  ///
  /// Content must be positive and contained in allocation. row_pitch must cover
  /// content.width * 4 bytes, and bytes must cover the final content row including
  /// its pixels; consumers check all arithmetic before accessing storage. The
  /// producer owns the bytes and keeps them readable through the consumer call.
  /// The consumer completes its copy before returning and retains no span.
  struct CpuRowsView {
    std::span<const std::byte> bytes;
    std::size_t row_pitch{};
    core::Extent content{}, allocation{};
  };

  /// @brief Borrowed external image registration for one renderer slot installation.
  ///
  /// Supply non-null handles created for the consuming Vulkan device, matching
  /// BGRA8 format/allocation and the producer's external layout/queue-family
  /// contract. The producer owns the image, view and backing memory and keeps all
  /// alive until renderer users retire and that ring's registrations are detached.
  /// Copying this value neither transfers ownership nor proves safe sampling.
  struct ExternalSampledImageView {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_B8G8R8A8_UNORM};
    core::Extent allocation{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_GENERAL};
    std::uint32_t external_queue_family{VK_QUEUE_FAMILY_EXTERNAL};
  };

  /// @brief Borrowed timeline wait for producer work already observed complete by the host.
  ///
  /// The producer confirms completion before publishing this value; it does not
  /// permit early submission while a source copy is pending. A non-null semaphore
  /// belongs to the consuming Vulkan device and the producer keeps it alive until
  /// submissions waiting on value retire. The queue wait preserves the external
  /// memory dependency; this value owns neither the semaphore nor a completion lease.
  struct CompletedProducerWait {
    VkSemaphore semaphore{VK_NULL_HANDLE};
    std::uint64_t value{};
  };

}    // namespace avb::render

#endif    // AVB0795_RENDER_VULKAN_INPUT_HPP
