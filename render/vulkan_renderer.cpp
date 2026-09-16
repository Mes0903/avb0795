/// @file
/// @brief Replica frame allocation, canonical slot transitions and submission.
/// Main-thread ticks coordinate device and WSI progress without synchronous waits.
#include "render/detail/vulkan_renderer_impl.hpp"
#include "avb/diagnostics/performance_report.hpp"

#include "render/graphics_retry_policy.hpp"
#include "render/render_math.hpp"
#include "render/vulkan_input.hpp"

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace avb::render {
  using diagnostics::PerformanceCounter;
  using diagnostics::PerformanceOwner;
  using diagnostics::PerformanceSample;
  using detail::cpu_slot_count;
  using detail::earlier_deadline;
  using detail::graphics_error;
  using detail::has_zero_dimension;
  using detail::MaintenanceVariant;
  using detail::ReplicaPushConstants;
  using detail::vulkan_error;

  namespace {

    using namespace std::chrono_literals;

    constexpr std::uint32_t bytes_per_pixel = 4;
    constexpr auto pending_poll_interval = 2ms;

    // Validate both native allocation size and host-addressable row arithmetic
    // before either side of the CPU bridge can use these dimensions.
    [[nodiscard]] std::optional<VkDeviceSize>
    checked_pixel_bytes(const core::Extent extent) noexcept
    {
      if (has_zero_dimension(extent)) {
        return std::nullopt;
      }
      constexpr auto maximum = (std::numeric_limits<VkDeviceSize>::max)();
      const auto width = static_cast<VkDeviceSize>(extent.width);
      const auto height = static_cast<VkDeviceSize>(extent.height);
      if (width > maximum / height || width * height > maximum / bytes_per_pixel) {
        return std::nullopt;
      }
      const auto bytes = width * height * bytes_per_pixel;
      if (bytes >
          static_cast<VkDeviceSize>((std::numeric_limits<std::size_t>::max)())) {
        return std::nullopt;
      }
      return bytes;
    }

  }    // namespace

  VulkanRenderer::Impl::Impl(GLFWwindow *const window,
                             const std::uint64_t generation,
                             const core::Extent framebuffer,
                             const VulkanStartupRequest &startup,
                             diagnostics::PerformanceReport *performance,
                             const std::uint64_t observation_instance)
      : window_{window}, performance_{performance}, observation_instance_{observation_instance}, generation_{generation}, framebuffer_{framebuffer}
  {
    if (window_ == nullptr || generation_ == 0) {
      throw graphics_error(core::AppErrorCode::InvalidValue);
    }
    // Every constructor operation precedes submission. Local native owners can
    // unwind immediately; the facade attaches external owners after return.
    try {
      create_vulkan_instance();
      create_surface();
      select_physical_device(startup);
      if (performance_) {
        std::snprintf(performance_driver_.data(), performance_driver_.size(),
                      "raw:%u", physical_properties_.driverVersion);
      }
      create_vulkan_device(startup);
      create_vulkan_common_resources();
      if (!has_zero_dimension(framebuffer_)) {
        if (const auto created = create_swapchain(framebuffer_); !created) {
          throw created.error();
        }
      }
      fully_constructed_ = true;
    } catch (...) {
      release_renderer_resources();
      throw;
    }
  }

  VulkanRenderer::Impl::~Impl() noexcept
  {
    detach_for_shutdown();
    release_renderer_resources();
  }

  void VulkanRenderer::Impl::detach_for_shutdown() noexcept
  {
    if (shutdown_detached_) return;
    if (fully_constructed_ && has_unsafe_native_work()) std::terminate();
    // Final teardown frees descriptors directly. Ordinary ring replacement uses
    // the fallible reset operation and preserves references when it fails.
    destroy_descriptor_pool();
    if (ring_serial_ != 0 && !detach_ring(ring_serial_)) std::terminate();
    shutdown_detached_ = true;
  }

  std::expected<std::uint64_t, core::AppError>
  VulkanRenderer::Impl::begin_ring(const core::Extent allocation) noexcept
  {
    if (ring_serial_ != 0 || !checked_pixel_bytes(allocation) ||
        vulkan_device_lost_ || shutdown_detached_) {
      return std::unexpected{graphics_error(core::AppErrorCode::InvalidValue)};
    }
    const auto serial = next_ring_serial(last_ring_serial_);
    if (!serial) {
      terminal_error_ = graphics_error(core::AppErrorCode::RuntimeStateLost);
      accepting_frames_ = false;
      return std::unexpected{*terminal_error_};
    }
    last_ring_serial_ = *serial;
    ring_serial_ = *serial;
    capture_allocation_ = allocation;
    ring_ready_ = false;
    for (std::size_t index{}; index < slots_.size(); ++index) {
      slots_[index].key = {*serial, static_cast<std::uint32_t>(index)};
    }
    return *serial;
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::publish_ring(const std::uint64_t serial,
                                     const std::uint64_t generation) noexcept
  {
    if (serial == 0 || serial != ring_serial_ || generation == 0 || ring_ready_ ||
        vulkan_device_lost_ ||
        !std::ranges::all_of(slots_, [](const CaptureSlot &slot) noexcept {
          return slot.phase == SlotPhase::Empty &&
                 slot.descriptor != VK_NULL_HANDLE &&
                 slot.command_buffer != VK_NULL_HANDLE &&
                 slot.acquire_fence != VK_NULL_HANDLE &&
                 slot.submission_fence != VK_NULL_HANDLE &&
                 slot.acquire_semaphore != VK_NULL_HANDLE &&
                 slot.render_finished_semaphore != VK_NULL_HANDLE &&
                 (slot.sampled_view != VK_NULL_HANDLE ||
                  slot.external_image.view != VK_NULL_HANDLE);
        })) {
      return std::unexpected{graphics_error(core::AppErrorCode::InvalidValue)};
    }
    generation_ = generation;
    ring_ready_ = true;
    return {};
  }

  void VulkanRenderer::Impl::discard_ready_inputs() noexcept
  {
    invalidate_presentation();
    retained_slot_ = nullptr;
    for (auto &slot : slots_) {
      if (slot.phase == SlotPhase::CpuReady || slot.phase == SlotPhase::GpuReady) {
        slot.phase = SlotPhase::Empty;
        slot.gpu_copy = false;
        slot.producer_wait = {};
        ++dropped_frames_;
        observe_performance(PerformanceCounter::Dropped);
        progress_pending_ = true;
      }
    }
  }

  void VulkanRenderer::Impl::begin_recovery_drain(const bool device_lost) noexcept
  {
    invalidate_presentation();
    accepting_frames_ = false;
    draining_ = true;
    retained_slot_ = nullptr;
    redraw_requested_ = false;
    if (device_lost) {
      vulkan_device_lost_ = true;
      progress_pending_ = progress_pending_ || presentations_in_flight() != 0;
      destroy_present_records();
    }
    for (auto &slot : slots_) {
      // Producer reservations survive even device loss until their owner reports
      // completion. Healthy native submissions retain their own proof.
      if (slot.phase == SlotPhase::ProducerPending ||
          (!device_lost && slot_has_vulkan_work(slot.phase))) continue;
      progress_pending_ = progress_pending_ || slot.phase != SlotPhase::Empty ||
                          slot.acquired_image;
      slot.phase = SlotPhase::Empty;
      slot.acquired_image = false;
    }
  }

  void VulkanRenderer::Impl::contain_failure(VulkanTickResult &result,
                                             const core::AppError error) noexcept
  {
    if (error.native_detail == static_cast<std::int64_t>(VK_ERROR_DEVICE_LOST)) {
      mark_vulkan_device_lost(result);
    }
    else {
      terminal_error_ = error;
      accepting_frames_ = false;
      result.terminal_error = error;
    }
  }

  VulkanTickResult VulkanRenderer::Impl::begin_tick(
    const core::Extent framebuffer,
    const std::chrono::steady_clock::time_point now) noexcept
  {
    VulkanTickResult result;
    result.made_progress = std::exchange(progress_pending_, false);
    if (framebuffer != framebuffer_) resize(framebuffer);
    result.presentation_suspended = has_zero_dimension(framebuffer_);
    result.vulkan_device_lost = vulkan_device_lost_;
    result.terminal_error = terminal_error_;
    try {
      progress_present_records(result, now);
    } catch (const core::AppError &error) {
      contain_failure(result, error);
    } catch (const std::bad_alloc &) {
      contain_failure(result, vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                           VK_ERROR_OUT_OF_HOST_MEMORY));
    } catch (...) {
      contain_failure(result, graphics_error(core::AppErrorCode::RuntimeStateLost));
    }
    result.vulkan_device_lost = vulkan_device_lost_;
    result.slots_in_flight = slots_in_flight();
    result.presentations_in_flight = presentations_in_flight();
    return result;
  }

  void VulkanRenderer::Impl::retire_submissions(
    VulkanTickResult &result, const std::chrono::steady_clock::time_point now) noexcept
  {
    try {
      progress_slots(result, now);
      for (auto &record : present_records_) {
        if (record && record->fence == VK_NULL_HANDLE) record.reset();
      }
    } catch (const core::AppError &error) {
      contain_failure(result, error);
    } catch (const std::bad_alloc &) {
      contain_failure(result, vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                           VK_ERROR_OUT_OF_HOST_MEMORY));
    } catch (...) {
      contain_failure(result, graphics_error(core::AppErrorCode::RuntimeStateLost));
    }
    result.vulkan_device_lost = vulkan_device_lost_;
    result.slots_in_flight = slots_in_flight();
    result.presentations_in_flight = presentations_in_flight();
  }

  void VulkanRenderer::Impl::finish_tick(
    VulkanTickResult &result, const std::chrono::steady_clock::time_point now, const bool allow_present, const bool allow_recovery) noexcept
  {
    try {
      if (allow_recovery && !vulkan_device_lost_) progress_recreation(result, now);
      if (allow_present && !draining_ && !terminal_error_ && !vulkan_device_lost_ &&
          !surface_recovery_pending_ && !swapchain_recreate_pending_ &&
          !has_zero_dimension(framebuffer_) && swapchain_) {
        if (auto *const slot = ready_slot()) {
          if (auto *const record = empty_present_record()) {
            try_present(*slot, *record, result, now);
            if (record->fence == VK_NULL_HANDLE) {
              const auto found = std::ranges::find_if(
                present_records_, [record](const auto &candidate) noexcept {
                  return candidate && &*candidate == record;
                });
              if (found != present_records_.end()) found->reset();
            }
          }
        }
      }
    } catch (const core::AppError &error) {
      contain_failure(result, error);
    } catch (const std::bad_alloc &) {
      contain_failure(result, vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                           VK_ERROR_OUT_OF_HOST_MEMORY));
    } catch (...) {
      contain_failure(result, graphics_error(core::AppErrorCode::RuntimeStateLost));
    }
    if (has_unsafe_native_work()) schedule_poll(result, now);
    result.made_progress = result.made_progress || std::exchange(progress_pending_, false);
    result.drain_complete = draining_ && slots_in_flight() == 0 &&
                            presentations_in_flight() == 0;
    result.cleanup_pending = surface_recovery_pending_ ||
                             swapchain_recreate_pending_ || !retired_swapchains_.empty();
    result.presented_frames = presented_frames_;
    result.dropped_frames = dropped_frames_;
    result.slots_in_flight = slots_in_flight();
    result.presentations_in_flight = presentations_in_flight();
    result.vulkan_device_lost = vulkan_device_lost_;
    result.terminal_error = terminal_error_;
    if (performance_) {
      performance_live_.slots = result.slots_in_flight;
      performance_live_.presents = result.presentations_in_flight;
      performance_->observe_high_water(PerformanceOwner::Replica, performance_live_,
                                       std::chrono::steady_clock::now());
    }
    if (!allow_present || draining_ || terminal_error_ || vulkan_device_lost_ ||
        surface_recovery_pending_ || swapchain_recreate_pending_ ||
        has_zero_dimension(framebuffer_)) invalidate_presentation();
    result.presentation = published_presentation_;
  }

  void VulkanRenderer::Impl::observe_metadata(
    const VulkanObservationContext context,
    const std::chrono::steady_clock::time_point now) noexcept
  {
    if (!performance_) return;
    std::uint32_t fps{};
    switch (context.fps) {
    case core::FpsCap::Fps30: fps = 30; break;
    case core::FpsCap::Fps60: fps = 60; break;
    case core::FpsCap::Fps144: fps = 144; break;
    case core::FpsCap::Unlimited: break;
    }
    const auto mode = swapchain_ ? swapchain_->present_mode : VK_PRESENT_MODE_FIFO_KHR;
    performance_->observe_runtime({observation_instance_, generation_}, now);
    performance_->observe_metadata(PerformanceOwner::Replica, {
                                                                .gpu = physical_properties_.deviceName,
                                                                .driver = performance_driver_.data(),
                                                                .os = context.os,
                                                                .bridge = context.bridge,
                                                                .bridge_reason = context.bridge_reason,
                                                                .present_mode = mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE"
                                                                                                                                                                        : "FIFO",
                                                                .ui_policy = "see controller owner metadata",
                                                                .requested_bridge = context.requested_bridge,
                                                                .content_width = context.content.width,
                                                                .content_height = context.content.height,
                                                                .allocation_width = capture_allocation_.width,
                                                                .allocation_height = capture_allocation_.height,
                                                                .framebuffer_width = framebuffer_.width,
                                                                .framebuffer_height = framebuffer_.height,
                                                                .fps_cap = fps,
                                                              },
                                   now);
    performance_live_.slots = slots_in_flight();
    performance_live_.presents = presentations_in_flight();
    performance_->observe_high_water(PerformanceOwner::Replica, performance_live_, now);
  }

  void VulkanRenderer::Impl::observe_performance(const PerformanceCounter counter,
                                                 const std::uint64_t value) noexcept
  {
    if (!performance_) return;
    switch (counter) {
    case PerformanceCounter::CommandBuffersCreated: performance_live_.command_buffers += value; break;
    case PerformanceCounter::CommandBuffersDestroyed: performance_live_.command_buffers -= value; break;
    case PerformanceCounter::FencesCreated: performance_live_.fences += value; break;
    case PerformanceCounter::FencesDestroyed: performance_live_.fences -= value; break;
    case PerformanceCounter::SemaphoresCreated: performance_live_.semaphores += value; break;
    case PerformanceCounter::SemaphoresDestroyed: performance_live_.semaphores -= value; break;
    default: break;
    }
    const auto now = std::chrono::steady_clock::now();
    performance_->observe_runtime({observation_instance_, generation_}, now);
    performance_->observe_counter(PerformanceOwner::Replica, counter, value, now);
    performance_live_.slots = slots_in_flight();
    performance_live_.presents = presentations_in_flight();
    performance_->observe_high_water(PerformanceOwner::Replica, performance_live_, now);
  }

  void VulkanRenderer::Impl::release_renderer_resources() noexcept
  {
    release_renderer_ring_resources();
    destroy_present_records();
    destroy_swapchain();
    destroy_retired_swapchains();
    destroy_vulkan_common_resources();
    if (device_ != VK_NULL_HANDLE) {
      vkDestroyDevice(device_, nullptr);
      device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
      vkDestroySurfaceKHR(instance_, surface_, nullptr);
      surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
      vkDestroyInstance(instance_, nullptr);
      instance_ = VK_NULL_HANDLE;
    }
  }



  std::expected<void, core::AppError>
  VulkanRenderer::Impl::create_buffer_memory(CaptureSlot &slot,
                                                 const VkDeviceSize size)
  {
    const VkBufferCreateInfo buffer_info{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = 0,
      .pQueueFamilyIndices = nullptr,
    };
    VkBuffer created_upload_buffer{VK_NULL_HANDLE};
    auto result =
      vkCreateBuffer(device_, &buffer_info, nullptr, &created_upload_buffer);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    slot.upload_buffer = created_upload_buffer;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, slot.upload_buffer, &requirements);
    const auto memory_index = find_memory_type(
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!memory_index) {
      return std::unexpected{
        graphics_error(core::AppErrorCode::UnsupportedGraphics)};
    }
    const auto memory_flags =
      memory_properties_.memoryTypes[*memory_index].propertyFlags;
    slot.upload_coherent =
      (memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    slot.upload_allocation_size = requirements.size;
    const VkMemoryAllocateInfo allocation_info{
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = nullptr,
      .allocationSize = requirements.size,
      .memoryTypeIndex = *memory_index,
    };
    VkDeviceMemory allocated_upload_memory{VK_NULL_HANDLE};
    result = vkAllocateMemory(device_, &allocation_info, nullptr,
                              &allocated_upload_memory);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    slot.upload_memory = allocated_upload_memory;
    result =
      vkBindBufferMemory(device_, slot.upload_buffer, slot.upload_memory, 0);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    void *mapped_upload{};
    result = vkMapMemory(device_, slot.upload_memory, 0, VK_WHOLE_SIZE, 0,
                         &mapped_upload);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    slot.mapped_upload = mapped_upload;
    return {};
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::create_image_memory(CaptureSlot &slot)
  {
    const VkImageCreateInfo image_info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent =
        {
          .width = capture_allocation_.width,
          .height = capture_allocation_.height,
          .depth = 1,
        },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = 0,
      .pQueueFamilyIndices = nullptr,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage created_sampled_image{VK_NULL_HANDLE};
    auto result =
      vkCreateImage(device_, &image_info, nullptr, &created_sampled_image);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    slot.sampled_image = created_sampled_image;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, slot.sampled_image, &requirements);
    const auto memory_index = find_memory_type(
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_index) {
      return std::unexpected{
        graphics_error(core::AppErrorCode::UnsupportedGraphics)};
    }
    const VkMemoryAllocateInfo allocation_info{
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = nullptr,
      .allocationSize = requirements.size,
      .memoryTypeIndex = *memory_index,
    };
    VkDeviceMemory allocated_sampled_memory{VK_NULL_HANDLE};
    result = vkAllocateMemory(device_, &allocation_info, nullptr,
                              &allocated_sampled_memory);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    slot.sampled_memory = allocated_sampled_memory;
    result =
      vkBindImageMemory(device_, slot.sampled_image, slot.sampled_memory, 0);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    const VkImageViewCreateInfo view_info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .image = slot.sampled_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .components = {},
      .subresourceRange =
        {
          .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
          .baseMipLevel = 0,
          .levelCount = 1,
          .baseArrayLayer = 0,
          .layerCount = 1,
        },
    };
    VkImageView created_sampled_view{VK_NULL_HANDLE};
    result =
      vkCreateImageView(device_, &view_info, nullptr, &created_sampled_view);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    slot.sampled_view = created_sampled_view;
    return {};
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::recreate_render_finished(CaptureSlot &slot)
  {
    if (slot.render_finished_semaphore != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, slot.render_finished_semaphore, nullptr);
      observe_performance(PerformanceCounter::SemaphoresDestroyed);
      slot.render_finished_semaphore = VK_NULL_HANDLE;
    }
    const VkSemaphoreCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
    };
    VkSemaphore created_render_finished_semaphore{VK_NULL_HANDLE};
    const auto result = vkCreateSemaphore(
      device_, &info, nullptr, &created_render_finished_semaphore);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    slot.render_finished_semaphore = created_render_finished_semaphore;
    observe_performance(PerformanceCounter::SemaphoresCreated);
    return {};
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::recreate_acquire_semaphore(CaptureSlot &slot)
  {
    if (slot.acquire_semaphore != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, slot.acquire_semaphore, nullptr);
      observe_performance(PerformanceCounter::SemaphoresDestroyed);
      slot.acquire_semaphore = VK_NULL_HANDLE;
    }
    const VkSemaphoreCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
    };
    VkSemaphore created_acquire_semaphore{VK_NULL_HANDLE};
    const auto result =
      vkCreateSemaphore(device_, &info, nullptr, &created_acquire_semaphore);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    slot.acquire_semaphore = created_acquire_semaphore;
    observe_performance(PerformanceCounter::SemaphoresCreated);
    return {};
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::create_slot_presentation_resources(
    CaptureSlot &slot, const VkImageView view, const VkImageLayout layout)
  {
    const VkDescriptorSetAllocateInfo descriptor_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .pNext = nullptr,
      .descriptorPool = descriptor_pool_,
      .descriptorSetCount = 1,
      .pSetLayouts = &descriptor_layout_,
    };
    VkDescriptorSet allocated_descriptor{VK_NULL_HANDLE};
    auto result =
      vkAllocateDescriptorSets(device_, &descriptor_info, &allocated_descriptor);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    slot.descriptor = allocated_descriptor;
    const VkDescriptorImageInfo image_info{
      .sampler = sampler_,
      .imageView = view,
      .imageLayout = layout,
    };
    const VkWriteDescriptorSet write{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .pNext = nullptr,
      .dstSet = slot.descriptor,
      .dstBinding = 0,
      .dstArrayElement = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &image_info,
      .pBufferInfo = nullptr,
      .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    const VkFenceCreateInfo fence_info{
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
    };
    VkFence created_acquire_fence{VK_NULL_HANDLE};
    result =
      vkCreateFence(device_, &fence_info, nullptr, &created_acquire_fence);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    slot.acquire_fence = created_acquire_fence;
    observe_performance(PerformanceCounter::FencesCreated);

    VkFence created_submission_fence{VK_NULL_HANDLE};
    result =
      vkCreateFence(device_, &fence_info, nullptr, &created_submission_fence);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    slot.submission_fence = created_submission_fence;
    observe_performance(PerformanceCounter::FencesCreated);
    if (auto created = recreate_acquire_semaphore(slot); !created) {
      return created;
    }
    return recreate_render_finished(slot);
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::create_capture_slot(CaptureSlot &slot)
  {
    const auto byte_count = checked_pixel_bytes(capture_allocation_);
    if (!byte_count) {
      return std::unexpected{graphics_error(core::AppErrorCode::InvalidValue)};
    }
    if (auto created = create_buffer_memory(slot, *byte_count); !created) {
      return created;
    }
    if (auto created = create_image_memory(slot); !created) {
      return created;
    }
    return create_slot_presentation_resources(
      slot, slot.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  void VulkanRenderer::Impl::destroy_capture_slot(
    CaptureSlot &slot) noexcept
  {
    // Pools span capture allocations. Preserve the command buffer for the next
    // ring while clearing every allocation-local owner and borrowed mapping.
    const auto command_buffer = slot.command_buffer;
    if (device_ != VK_NULL_HANDLE) {
      if (slot.render_finished_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, slot.render_finished_semaphore, nullptr);
        observe_performance(PerformanceCounter::SemaphoresDestroyed);
      }
      if (slot.acquire_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, slot.acquire_semaphore, nullptr);
        observe_performance(PerformanceCounter::SemaphoresDestroyed);
      }
      if (slot.submission_fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, slot.submission_fence, nullptr);
        observe_performance(PerformanceCounter::FencesDestroyed);
      }
      if (slot.acquire_fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, slot.acquire_fence, nullptr);
        observe_performance(PerformanceCounter::FencesDestroyed);
      }
      if (slot.sampled_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, slot.sampled_view, nullptr);
      }
      if (slot.sampled_image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, slot.sampled_image, nullptr);
      }
      if (slot.sampled_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, slot.sampled_memory, nullptr);
      }
      if (slot.mapped_upload != nullptr && slot.upload_memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device_, slot.upload_memory);
      }
      if (slot.upload_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, slot.upload_buffer, nullptr);
      }
      if (slot.upload_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, slot.upload_memory, nullptr);
      }
    }
    slot = CaptureSlot{};
    slot.command_buffer = command_buffer;
  }


  std::expected<void, core::AppError>
  VulkanRenderer::Impl::detach_ring(
    const std::uint64_t expected_ring_serial) noexcept
  {
    if (expected_ring_serial == 0 || expected_ring_serial != ring_serial_) {
      return std::unexpected{graphics_error(core::AppErrorCode::InvalidValue)};
    }
    // Validate every identity and allocation-user proof before withdrawing even
    // a retained pointer. Vulkan loss cannot discharge a producer reservation.
    for (std::size_t index{}; index < slots_.size(); ++index) {
      const auto &slot = slots_[index];
      if (slot.key != SlotKey{expected_ring_serial, static_cast<std::uint32_t>(index)} ||
          slot.phase == SlotPhase::ProducerPending ||
          (!vulkan_device_lost_ &&
           (slot.acquired_image ||
            (slot.phase != SlotPhase::Empty && slot.phase != SlotPhase::CpuReady &&
             slot.phase != SlotPhase::GpuReady)))) {
        return std::unexpected{graphics_error(core::AppErrorCode::RuntimeStateLost)};
      }
    }
    if (device_ != VK_NULL_HANDLE && descriptor_pool_ != VK_NULL_HANDLE) {
      const auto reset = vkResetDescriptorPool(device_, descriptor_pool_, 0);
      if (reset != VK_SUCCESS) {
        return std::unexpected{vulkan_error(
          core::AppErrorCode::RuntimeConstructionFailure, reset)};
      }
    }
    ring_ready_ = false;
    retained_slot_ = nullptr;
    for (auto &slot : slots_) {
      slot.key = {};
      slot.descriptor = VK_NULL_HANDLE;
      slot.external_image = {};
      slot.producer_wait = {};
      slot.restore_retained_on_cancel = false;
      slot.gpu_copy = false;
      slot.phase = SlotPhase::Empty;
      slot.acquired_image = false;
      slot.owning_swapchain = VK_NULL_HANDLE;
    }
    return {};
  }

  void VulkanRenderer::Impl::release_renderer_ring_resources() noexcept
  {
    for (auto &slot : slots_) {
      destroy_capture_slot(slot);
    }
    ring_serial_ = 0;
    ring_ready_ = false;
    capture_allocation_ = {};
  }

  bool VulkanRenderer::Impl::ready_for_frame() const noexcept
  {
    return accepting_frames_ && !terminal_error_ &&
           !vulkan_device_lost_ && ring_ready_ && ring_serial_ != 0 &&
           std::ranges::any_of(slots_, [](const CaptureSlot &slot) noexcept {
             return slot.phase == SlotPhase::Empty;
           });
  }

  VulkanRenderer::Impl::CaptureSlot *
  VulkanRenderer::Impl::empty_slot() noexcept
  {
    const auto found = std::ranges::find_if(slots_, [this](const CaptureSlot &slot) {
      return slot.phase == SlotPhase::Empty && &slot != retained_slot_;
    });
    if (found != slots_.end()) {
      return &*found;
    }
    return retained_slot_ && retained_slot_->phase == SlotPhase::Empty
             ? retained_slot_
             : nullptr;
  }

  std::optional<SlotKey> VulkanRenderer::Impl::reserve_source_slot() noexcept
  {
    if (!ready_for_frame()) {
      return std::nullopt;
    }
    auto *const slot = empty_slot();
    if (slot == nullptr || find_slot(slot->key, SlotPhase::Empty) != slot) {
      return std::nullopt;
    }
    slot->restore_retained_on_cancel = slot == retained_slot_;
    if (slot->restore_retained_on_cancel) {
      retained_slot_ = nullptr;
    }
    slot->phase = SlotPhase::ProducerPending;
    return slot->key;
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::cancel_source_reservation(const SlotKey key) noexcept
  {
    auto *const slot = find_slot(key, SlotPhase::ProducerPending);
    if (slot == nullptr) {
      return std::unexpected{graphics_error(core::AppErrorCode::InvalidValue)};
    }
    slot->phase = SlotPhase::Empty;
    if (slot->restore_retained_on_cancel && ring_ready_ && accepting_frames_ &&
        !draining_ && !terminal_error_ && !vulkan_device_lost_ &&
        slot->stamp.sequence >= last_submitted_sequence_ &&
        (!retained_slot_ || slot->stamp.sequence >= retained_slot_->stamp.sequence)) {
      retained_slot_ = slot;
    }
    slot->restore_retained_on_cancel = false;
    return {};
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::register_external_image(
    const SlotKey key, const ExternalSampledImageView image) noexcept
  {
    auto *const slot = find_slot(key, SlotPhase::Empty);
    if (slot == nullptr || ring_ready_ || image.image == VK_NULL_HANDLE ||
        image.view == VK_NULL_HANDLE || image.format != VK_FORMAT_B8G8R8A8_UNORM ||
        image.allocation != capture_allocation_ || !checked_pixel_bytes(image.allocation) ||
        slot->external_image.image != VK_NULL_HANDLE ||
        slot->sampled_image != VK_NULL_HANDLE || slot->upload_buffer != VK_NULL_HANDLE ||
        slot->descriptor != VK_NULL_HANDLE || vulkan_device_lost_) {
      return std::unexpected{graphics_error(core::AppErrorCode::InvalidValue)};
    }
    slot->external_image = image;
    return create_slot_presentation_resources(*slot, image.view, image.layout);
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::complete_external_input(
    const SlotKey key, const core::Extent content, const FrameStamp stamp,
    const CompletedProducerWait wait) noexcept
  {
    auto *const slot = find_slot(key, SlotPhase::ProducerPending);
    if (slot == nullptr || !ring_ready_ ||
        slot->external_image.image == VK_NULL_HANDLE ||
        slot->external_image.view == VK_NULL_HANDLE ||
        slot->external_image.format != VK_FORMAT_B8G8R8A8_UNORM ||
        slot->external_image.allocation != capture_allocation_ ||
        !checked_pixel_bytes(content) || content.width > capture_allocation_.width ||
        content.height > capture_allocation_.height ||
        wait.semaphore == VK_NULL_HANDLE || wait.value == 0 ||
        draining_ || vulkan_device_lost_) {
      return std::unexpected{graphics_error(core::AppErrorCode::RuntimeStateLost)};
    }
    slot->content = content;
    slot->stamp = stamp;
    slot->producer_wait = wait;
    slot->gpu_copy = true;
    slot->restore_retained_on_cancel = false;
    slot->phase = SlotPhase::GpuReady;
    return {};
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::discard_completed_source(const SlotKey key) noexcept
  {
    auto *const slot = find_slot(key, SlotPhase::ProducerPending);
    if (slot == nullptr) {
      return std::unexpected{graphics_error(core::AppErrorCode::InvalidValue)};
    }
    slot->phase = SlotPhase::Empty;
    slot->restore_retained_on_cancel = false;
    slot->gpu_copy = false;
    slot->producer_wait = {};
    return {};
  }

  VulkanRenderer::Impl::CaptureSlot *
  VulkanRenderer::Impl::find_slot(const SlotKey key,
                                     const SlotPhase phase) noexcept
  {
    if (!matches_slot_key(key, ring_serial_, slots_.size())) {
      return nullptr;
    }
    auto &slot = slots_[key.index];
    return slot.key == key && slot.phase == phase ? &slot : nullptr;
  }

  bool VulkanRenderer::Impl::has_unsafe_native_work() const noexcept
  {
    if (std::ranges::any_of(slots_, [](const CaptureSlot &slot) noexcept {
          return slot.phase == SlotPhase::ProducerPending;
        })) {
      return true;
    }
    if (vulkan_device_lost_) {
      return false;
    }
    if (std::ranges::any_of(present_records_, [](const auto &record) noexcept {
          return record.has_value();
        })) {
      return true;
    }
    return std::ranges::any_of(slots_, [](const CaptureSlot &slot) noexcept {
      return slot_has_vulkan_work(slot.phase);
    });
  }

  std::size_t VulkanRenderer::Impl::slots_in_flight() const noexcept
  {
    return static_cast<std::size_t>(
      std::ranges::count_if(slots_, [](const CaptureSlot &slot) noexcept {
        return slot.phase != SlotPhase::Empty;
      }));
  }

  std::size_t VulkanRenderer::Impl::presentations_in_flight() const noexcept
  {
    return static_cast<std::size_t>(std::ranges::count_if(
      present_records_,
      [](const std::optional<PresentRecord> &record) noexcept {
        return record.has_value();
      }));
  }

  bool VulkanRenderer::Impl::render_allocation_idle() const noexcept
  {
    // A pending present owns its semaphore and swapchain in a separate record.
    // Once slot submission retires it no longer references the capture image or
    // descriptor, so those allocation-local resources can be reconstructed.
    return std::ranges::all_of(slots_, [](const CaptureSlot &slot) noexcept {
      return slot.phase == SlotPhase::Empty ||
             slot.phase == SlotPhase::CpuReady ||
             slot.phase == SlotPhase::GpuReady;
    });
  }

  VulkanRenderer::Impl::CaptureSlot *
  VulkanRenderer::Impl::ready_slot() noexcept
  {
    std::array<RenderFrameCandidate, cpu_slot_count> candidates{};
    for (std::size_t index{}; index < slots_.size(); ++index) {
      const auto &slot = slots_[index];
      candidates[index] = {slot.phase, slot.stamp.sequence,
                           &slot == retained_slot_};
    }
    const auto selected = select_render_frame(
      candidates, last_submitted_sequence_, redraw_requested_);
    auto *const newest = selected ? &slots_[*selected] : nullptr;
    for (auto &slot : slots_) {
      if (&slot != newest && (slot.phase == SlotPhase::CpuReady ||
                              slot.phase == SlotPhase::GpuReady)) {
        // producer completion has already checked in the source frame, and Vulkan has
        // not used this slot. Dropping stale ready content needs no native wait.
        slot.phase = SlotPhase::Empty;
        ++dropped_frames_;
        observe_performance(PerformanceCounter::Dropped);
      }
    }
    return newest;
  }

  void VulkanRenderer::Impl::set_presentation(
    const ReplicaPresentation presentation) noexcept
  {
    if (presentation == presentation_) {
      return;
    }
    if (presentation.selecting_region != presentation_.selecting_region) {
      invalidate_presentation();
    }
    presentation_ = presentation;
    presentation_dirty_ = true;
  }

  bool VulkanRenderer::Impl::has_redraw_work() const noexcept
  {
    return presentation_dirty_ && retained_slot_ && !draining_ &&
           !terminal_error_ && !vulkan_device_lost_ &&
           !has_zero_dimension(framebuffer_);
  }

  void VulkanRenderer::Impl::invalidate_presentation() noexcept
  {
    published_presentation_.reset();
    first_valid_present_sequence_ = next_present_sequence_;
  }

  void VulkanRenderer::Impl::begin_drain() noexcept
  {
    invalidate_presentation();
    accepting_frames_ = false;
    draining_ = true;
    retained_slot_ = nullptr;
    redraw_requested_ = false;
    for (auto &slot : slots_) {
      if (slot.phase == SlotPhase::CpuReady ||
          slot.phase == SlotPhase::GpuReady) {
        slot.phase = SlotPhase::Empty;
        ++dropped_frames_;
        observe_performance(PerformanceCounter::Dropped);
        progress_pending_ = true;
      }
    }
  }

  void VulkanRenderer::Impl::mark_vulkan_device_lost(
    VulkanTickResult &result) noexcept
  {
    vulkan_device_lost_ = true;
    accepting_frames_ = false;
    result.vulkan_device_lost = true;
  }

  void VulkanRenderer::Impl::set_terminal(
    const VkResult result, VulkanTickResult &tick_result) noexcept
  {
    terminal_error_ = vulkan_error(core::AppErrorCode::RuntimeStateLost, result);
    accepting_frames_ = false;
    tick_result.terminal_error = terminal_error_;
  }

  void VulkanRenderer::Impl::schedule_poll(
    VulkanTickResult &result,
    const std::chrono::steady_clock::time_point now) const noexcept
  {
    earlier_deadline(result.next_deadline, now + pending_poll_interval);
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::complete_cpu_input(
    const SlotKey key, const CpuRowsView rows, const FrameStamp stamp) noexcept
  {
    auto *const matched = find_slot(key, SlotPhase::ProducerPending);
    const auto byte_count = checked_pixel_bytes(capture_allocation_);
    if (matched == nullptr || !ring_ready_ || !byte_count || !checked_pixel_bytes(rows.content) ||
        rows.allocation != capture_allocation_ ||
        rows.content.width > rows.allocation.width ||
        rows.content.height > rows.allocation.height ||
        matched->external_image.image != VK_NULL_HANDLE ||
        matched->upload_buffer == VK_NULL_HANDLE ||
        matched->upload_memory == VK_NULL_HANDLE ||
        matched->sampled_image == VK_NULL_HANDLE ||
        matched->sampled_view == VK_NULL_HANDLE ||
        matched->mapped_upload == nullptr ||
        *byte_count > matched->upload_allocation_size ||
        draining_ || vulkan_device_lost_) {
      return std::unexpected{graphics_error(core::AppErrorCode::RuntimeStateLost)};
    }
    auto &slot = *matched;
    const auto row_bytes = static_cast<std::size_t>(rows.content.width) * bytes_per_pixel;
    const auto last_row = static_cast<std::size_t>(rows.content.height - 1);
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    if (rows.bytes.data() == nullptr || rows.row_pitch < row_bytes ||
        (last_row != 0 && rows.row_pitch > (maximum - row_bytes) / last_row) ||
        rows.bytes.size() < last_row * rows.row_pitch + row_bytes) {
      return std::unexpected{graphics_error(core::AppErrorCode::RuntimeStateLost)};
    }
    // Upload commands copy the entire allocation, but capture only defines the
    // content rectangle. Clear the remaining bytes and translate producer's row pitch
    // into the tightly packed allocation stride expected by Vulkan's buffer copy.
    std::memset(slot.mapped_upload, 0, static_cast<std::size_t>(*byte_count));
    observe_performance(PerformanceCounter::CpuClearBytes, *byte_count);
    auto *const destination = static_cast<std::byte *>(slot.mapped_upload);
    const auto *const source = rows.bytes.data();
    const auto destination_pitch =
      static_cast<std::size_t>(capture_allocation_.width) * bytes_per_pixel;
    for (std::uint32_t row = 0; row < rows.content.height; ++row) {
      std::memcpy(destination + static_cast<std::size_t>(row) * destination_pitch,
                  source + static_cast<std::size_t>(row) * rows.row_pitch,
                  row_bytes);
    }

    observe_performance(PerformanceCounter::CpuCopyBytes, static_cast<std::uint64_t>(row_bytes) * rows.content.height);

    if (!slot.upload_coherent) {
      // Flush whole noncoherent atoms. If rounding would overflow or extend past
      // the allocation, VK_WHOLE_SIZE covers the mapped allocation's remainder.
      const auto atom_size =
        (std::max) (physical_properties_.limits.nonCoherentAtomSize,
                    VkDeviceSize{1});
      const auto rounded_size =
        *byte_count >
            (std::numeric_limits<VkDeviceSize>::max)() - (atom_size - 1)
          ? VK_WHOLE_SIZE
          : ((*byte_count + atom_size - 1) / atom_size) * atom_size;
      const auto flush_size = rounded_size == VK_WHOLE_SIZE ||
                                  rounded_size > slot.upload_allocation_size
                                ? VK_WHOLE_SIZE
                                : rounded_size;
      const VkMappedMemoryRange range{
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .pNext = nullptr,
        .memory = slot.upload_memory,
        .offset = 0,
        .size = flush_size,
      };
      const auto flush_result = vkFlushMappedMemoryRanges(device_, 1, &range);
      if (flush_result == VK_ERROR_DEVICE_LOST) {
        // Latch loss before the bridge can visit another input and before this
        // tick reaches any later Vulkan operation. The bridge discards its rest.
        vulkan_device_lost_ = true;
        accepting_frames_ = false;
      }
      if (flush_result != VK_SUCCESS) {
        return std::unexpected{vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                            flush_result)};
      }
    }

    slot.content = rows.content;
    slot.stamp = stamp;
    slot.producer_wait = {};
    slot.gpu_copy = false;
    slot.restore_retained_on_cancel = false;
    slot.phase = SlotPhase::CpuReady;
    return {};
  }

  VkResult VulkanRenderer::Impl::record_commands(
    CaptureSlot &slot, const std::uint32_t image_index) noexcept
  {
    if (!swapchain_ || image_index >= swapchain_->framebuffers.size()) {
      return VK_ERROR_UNKNOWN;
    }
    auto result = vkResetCommandBuffer(slot.command_buffer, 0);
    if (result != VK_SUCCESS) {
      return result;
    }
    const VkCommandBufferBeginInfo begin_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .pNext = nullptr,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      .pInheritanceInfo = nullptr,
    };
    result = vkBeginCommandBuffer(slot.command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
      return result;
    }

    if (!slot.gpu_copy && &slot != retained_slot_) {
      // Reused CPU storage already has a shader-read layout; first use does not.
      // Retained redraw bypasses this upload because its sampled pixels survive
      // submission retirement and no new host data has replaced them.
      const VkImageMemoryBarrier before_copy{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = slot.sampled_image_initialized
                           ? VK_ACCESS_SHADER_READ_BIT
                           : VkAccessFlags{0},
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = slot.sampled_image_initialized
                       ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                       : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = slot.sampled_image,
        .subresourceRange =
          {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
          },
      };
      const auto source_stage = slot.sampled_image_initialized
                                  ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      vkCmdPipelineBarrier(slot.command_buffer, source_stage,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &before_copy);

      const VkBufferImageCopy copy{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
          {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
          },
        .imageOffset = {},
        .imageExtent =
          {
            .width = capture_allocation_.width,
            .height = capture_allocation_.height,
            .depth = 1,
          },
      };
      vkCmdCopyBufferToImage(slot.command_buffer, slot.upload_buffer,
                             slot.sampled_image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      observe_performance(PerformanceCounter::VulkanUploadBytes, static_cast<std::uint64_t>(capture_allocation_.width) * capture_allocation_.height * bytes_per_pixel);

      const VkImageMemoryBarrier before_sampling{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = slot.sampled_image,
        .subresourceRange =
          {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
          },
      };
      vkCmdPipelineBarrier(slot.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &before_sampling);
    }
    else if (slot.gpu_copy) {
      // producer completion supplies the timeline ordering, while this barrier acquires
      // the shared image for Vulkan sampling. Both are needed on retained redraw
      // too, since the prior command sequence returned ownership to EXTERNAL.
      const VkImageMemoryBarrier acquire_external{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = slot.external_image.layout,
        .newLayout = slot.external_image.layout,
        .srcQueueFamilyIndex = slot.external_image.external_queue_family,
        .dstQueueFamilyIndex = graphics_queue_family_,
        .image = slot.external_image.image,
        .subresourceRange =
          {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
          },
      };
      vkCmdPipelineBarrier(slot.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &acquire_external);
    }

    // Clear the whole attachment before narrowing viewport/scissor to content.
    // Letterbox pixels therefore stay opaque black across resize and redraw.
    constexpr VkClearValue clear{.color = {{0.0F, 0.0F, 0.0F, 1.0F}}};
    const VkRenderPassBeginInfo render_pass_info{
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .pNext = nullptr,
      .renderPass = swapchain_->render_pass,
      .framebuffer = swapchain_->framebuffers[image_index],
      .renderArea = {.offset = {}, .extent = swapchain_->extent},
      .clearValueCount = 1,
      .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(slot.command_buffer, &render_pass_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    const core::Extent destination{swapchain_->extent.width, swapchain_->extent.height};
    const auto sampling = region_sampling(
      presentation_.selecting_region ? std::nullopt : presentation_.region,
      slot.content, capture_allocation_, destination);
    if (!sampling) {
      vkCmdEndRenderPass(slot.command_buffer);
      static_cast<void>(vkEndCommandBuffer(slot.command_buffer));
      return VK_ERROR_UNKNOWN;
    }
    const auto *const fitted = &sampling->viewport;
    slot.recorded_presentation = {
      slot.content, destination, *fitted, sampling->region,
      presentation_.selecting_region, 0};
    const VkViewport viewport{
      .x = static_cast<float>(fitted->x),
      .y = static_cast<float>(fitted->y),
      .width = static_cast<float>(fitted->width),
      .height = static_cast<float>(fitted->height),
      .minDepth = 0.0F,
      .maxDepth = 1.0F,
    };
    const VkRect2D scissor{
      .offset = {.x = static_cast<std::int32_t>(fitted->x),
                 .y = static_cast<std::int32_t>(fitted->y)},
      .extent = {.width = fitted->width, .height = fitted->height},
    };
    ReplicaPushConstants constants{
      .transform = {sampling->texture_offset[0], sampling->texture_offset[1],
                    sampling->texture_scale[0], sampling->texture_scale[1]},
      .bounds = {sampling->texture_min[0], sampling->texture_min[1],
                 sampling->texture_max[0], sampling->texture_max[1]},
      .selection = {},
      .stroke = {},
    };
    if (presentation_.selecting_region && presentation_.selection_preview) {
      if (const auto preview = core::intersect_region(*presentation_.selection_preview, slot.content)) {
        const auto width = static_cast<float>(slot.content.width);
        const auto height = static_cast<float>(slot.content.height);
        constants.selection = {
          static_cast<float>(preview->x) / width,
          static_cast<float>(preview->y) / height,
          static_cast<float>(preview->x + preview->width) / width,
          static_cast<float>(preview->y + preview->height) / height};
        constants.stroke = {1.0F, 2.0F / static_cast<float>(fitted->width),
                            2.0F / static_cast<float>(fitted->height), 0.0F};
      }
    }
    vkCmdSetViewport(slot.command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(slot.command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(slot.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      swapchain_->pipeline);
    vkCmdBindDescriptorSets(slot.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_, 0, 1, &slot.descriptor, 0, nullptr);
    vkCmdPushConstants(
      slot.command_buffer, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
      static_cast<std::uint32_t>(sizeof(constants)), &constants);
    vkCmdDraw(slot.command_buffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(slot.command_buffer);
    if (slot.gpu_copy) {
      const VkImageMemoryBarrier release_external{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = 0,
        .oldLayout = slot.external_image.layout,
        .newLayout = slot.external_image.layout,
        .srcQueueFamilyIndex = graphics_queue_family_,
        .dstQueueFamilyIndex = slot.external_image.external_queue_family,
        .image = slot.external_image.image,
        .subresourceRange =
          {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
          },
      };
      vkCmdPipelineBarrier(slot.command_buffer,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &release_external);
    }
    // The submission fence also covers the external release barrier. Slot reuse
    // waits for that proof before allowing a later producer copy to overwrite the image.
    return vkEndCommandBuffer(slot.command_buffer);
  }

  void VulkanRenderer::Impl::reset_slot_after_vulkan(CaptureSlot &slot)
  {
    // Every caller reaches here only after native use was proven complete (or
    // after the owning swapchain was legally destroyed). A synchronization
    // reset/allocation failure therefore makes the slot unusable, not in flight.
    const bool completed_content = slot.phase == SlotPhase::Submitted;
    slot.acquired_image = false;
    slot.owning_swapchain = VK_NULL_HANDLE;
    slot.phase = SlotPhase::Empty;
    const std::array fences{slot.acquire_fence, slot.submission_fence};
    const auto reset_result = vkResetFences(
      device_, static_cast<std::uint32_t>(fences.size()), fences.data());
    if (reset_result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeStateLost, reset_result);
    }
    if (slot.acquire_semaphore == VK_NULL_HANDLE) {
      if (auto created = recreate_acquire_semaphore(slot); !created) {
        throw core::AppError{.code = core::AppErrorCode::RuntimeStateLost,
                             .native_detail = created.error().native_detail};
      }
    }
    if (slot.render_finished_semaphore == VK_NULL_HANDLE) {
      if (auto created = recreate_render_finished(slot); !created) {
        throw core::AppError{.code = core::AppErrorCode::RuntimeStateLost,
                             .native_detail = created.error().native_detail};
      }
    }
    if (completed_content && !draining_ && !terminal_error_ &&
        accepting_frames_ &&
        (!retained_slot_ ||
         slot.stamp.sequence >= retained_slot_->stamp.sequence)) {
      // Retain only our completed sampled image, never a source checkout. GPU
      // replay reacquires it from EXTERNAL and waits on the same completed producer
      // timeline value; no producer writer touches this slot before it is reused.
      retained_slot_ = &slot;
    }
  }

  void VulkanRenderer::Impl::progress_submission(
    CaptureSlot &slot, VulkanTickResult &result, const std::chrono::steady_clock::time_point now)
  {
    VkFence proof_fence{VK_NULL_HANDLE};
    switch (proof_fence_for_slot(slot.phase)) {
    case SlotProofFence::Acquire:
      proof_fence = slot.acquire_fence;
      break;
    case SlotProofFence::Submission:
      proof_fence = slot.submission_fence;
      break;
    case SlotProofFence::None:
      return;
    }
    auto status = vkGetFenceStatus(device_, proof_fence);
    if (status == VK_SUCCESS && proof_fence != slot.acquire_fence) {
      // Acquisition signals its fence after its semaphore. A submission waiting
      // on that semaphore can finish while the acquire fence signal is still
      // pending, so observe both fences before resetting the reusable slot.
      status = vkGetFenceStatus(device_, slot.acquire_fence);
    }
    if (status == VK_NOT_READY) {
      schedule_poll(result, now);
      return;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
      mark_vulkan_device_lost(result);
      return;
    }
    if (status != VK_SUCCESS) {
      set_terminal(status, result);
      schedule_poll(result, now);
      return;
    }

    if (slot.phase == SlotPhase::Submitted) {
      // Presentation, if still pending, holds its own record and wait semaphore.
      // This proof retires only the slot's command-buffer and sampled-image use.
      result.made_progress = true;
      reset_slot_after_vulkan(slot);
      return;
    }
    release_acquired_image(slot, result);
  }

  void VulkanRenderer::Impl::progress_slots(
    VulkanTickResult &result, const std::chrono::steady_clock::time_point now)
  {
    for (auto &slot : slots_) {
      switch (slot.phase) {
      case SlotPhase::ProducerPending:
        break;
      case SlotPhase::Submitted:
      case SlotPhase::PresentNotEnqueued:
      case SlotPhase::AcquireProofPending:
      case SlotPhase::SubmitUncertain:
        if (!vulkan_device_lost_) {
          progress_submission(slot, result, now);
        }
        break;
      case SlotPhase::Empty:
      case SlotPhase::CpuReady:
      case SlotPhase::GpuReady:
      case SlotPhase::ReleaseRetained:
        break;
      }
    }
  }

  void VulkanRenderer::Impl::try_present(
    CaptureSlot &slot, PresentRecord &record, VulkanTickResult &result, const std::chrono::steady_clock::time_point now)
  {
    std::uint32_t image_index{(std::numeric_limits<std::uint32_t>::max)()};
    const auto acquire_result = vkAcquireNextImageKHR(
      device_, swapchain_->handle, 0, slot.acquire_semaphore,
      slot.acquire_fence, &image_index);
    const auto acquire = classify_acquire_result(acquire_result, false);
    switch (acquire.action) {
    case AcquireAction::Wait:
      schedule_poll(result, now);
      return;
    case AcquireAction::RecreateSwapchain:
      swapchain_recreate_pending_ = true;
      schedule_poll(result, now);
      return;
    case AcquireAction::RecreateSurface:
      if (!surface_recovery_active_) {
        result.surface_recovery_started = true;
      }
      surface_recovery_active_ = true;
      surface_recovery_pending_ = true;
      schedule_poll(result, now);
      return;
    case AcquireAction::RebuildDevice:
      mark_vulkan_device_lost(result);
      return;
    case AcquireAction::Terminal:
      set_terminal(acquire_result, result);
      return;
    case AcquireAction::UseImage:
      break;
    }

    // Acquire has created ownership before command recording can fail. Record it
    // now so every later error retains the correct image and acquire proof.
    slot.acquired_image = true;
    slot.owning_swapchain = swapchain_->handle;
    slot.acquired_image_index = image_index;
    const auto command_result = record_commands(slot, image_index);
    if (command_result != VK_SUCCESS) {
      slot.phase = SlotPhase::AcquireProofPending;
      if (command_result == VK_ERROR_DEVICE_LOST) {
        mark_vulkan_device_lost(result);
        return;
      }
      set_terminal(command_result, result);
      schedule_poll(result, now);
      return;
    }

    // WSI availability gates attachment writes; shared producer data gates sampling.
    // The CPU path uses only the acquire wait and has no imported timeline.
    const std::array wait_semaphores{slot.acquire_semaphore, slot.producer_wait.semaphore};
    const std::array<VkPipelineStageFlags, 2> wait_stages{
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
    const std::array<std::uint64_t, 2> wait_values{0, slot.producer_wait.value};
    constexpr std::array<std::uint64_t, 1> signal_values{0};
    const VkTimelineSemaphoreSubmitInfo timeline_submit{
      .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
      .pNext = nullptr,
      .waitSemaphoreValueCount = 2,
      .pWaitSemaphoreValues = wait_values.data(),
      .signalSemaphoreValueCount = 1,
      .pSignalSemaphoreValues = signal_values.data(),
    };
    const bool gpu_submit = slot.gpu_copy;
    const VkSubmitInfo submit_info{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .pNext = gpu_submit ? &timeline_submit : nullptr,
      .waitSemaphoreCount = gpu_submit ? 2U : 1U,
      .pWaitSemaphores = wait_semaphores.data(),
      .pWaitDstStageMask = wait_stages.data(),
      .commandBufferCount = 1,
      .pCommandBuffers = &slot.command_buffer,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &slot.render_finished_semaphore,
    };
    // The producer timeline target has already completed before this submission is
    // attempted; no external wait can depend on later main-loop progress.
    const auto submit_result =
      vkQueueSubmit(graphics_queue_, 1, &submit_info, slot.submission_fence);
    const auto submit = classify_submit_result(submit_result);
    if (const auto phase = slot_phase_after_submit(submit)) {
      slot.phase = *phase;
    }
    switch (submit) {
    case SubmitOutcome::NotEnqueued:
      set_terminal(submit_result, result);
      schedule_poll(result, now);
      return;
    case SubmitOutcome::DeviceLost:
      mark_vulkan_device_lost(result);
      return;
    case SubmitOutcome::Uncertain:
      set_terminal(submit_result, result);
      schedule_poll(result, now);
      return;
    case SubmitOutcome::Enqueued:
      if (performance_) {
        const auto submitted_at = std::chrono::steady_clock::now();
        performance_->observe_runtime({observation_instance_, generation_}, submitted_at);
        performance_->observe_counter(PerformanceOwner::Replica, PerformanceCounter::Submitted, 1, submitted_at);
        if (slot.stamp.sequence > last_submitted_sequence_) {
          performance_->observe_sample(PerformanceSample::IntakeToSubmit,
            std::chrono::duration_cast<std::chrono::nanoseconds>(submitted_at - slot.stamp.intake_observed_at), submitted_at);
        }
        else {
          performance_->observe_counter(PerformanceOwner::Replica, PerformanceCounter::RetainedRedraws, 1, submitted_at);
        }
      }
      last_submitted_sequence_ = slot.stamp.sequence;
      if (gpu_submit) {
        ++gpu_interop_frames_submitted_;
      }
      else {
        slot.sampled_image_initialized = true;
      }
      break;
    }

    const VkFenceCreateInfo fence_info{
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
    };
    VkFence present_fence{VK_NULL_HANDLE};
    auto present_result =
      vkCreateFence(device_, &fence_info, nullptr, &present_fence);
    if (present_result != VK_SUCCESS) {
      slot.phase = slot_phase_after_present(PresentOwnership::NotEnqueued);
      if (present_result == VK_ERROR_DEVICE_LOST) {
        mark_vulkan_device_lost(result);
        return;
      }
      set_terminal(present_result, result);
      schedule_poll(result, now);
      return;
    }

    observe_performance(PerformanceCounter::FencesCreated);
#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
    const VkSwapchainPresentFenceInfoKHR present_fence_khr{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
      .pNext = nullptr,
      .swapchainCount = 1,
      .pFences = &present_fence,
    };
#endif
#if defined(AVB0795_HAS_EXT_MAINTENANCE_VARIANT)
    const VkSwapchainPresentFenceInfoEXT present_fence_ext{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
      .pNext = nullptr,
      .swapchainCount = 1,
      .pFences = &present_fence,
    };
#endif
    const void *present_fence_info{};
    switch (maintenance_variant_) {
    case MaintenanceVariant::Khr:
#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
      present_fence_info = &present_fence_khr;
#endif
      break;
    case MaintenanceVariant::Ext:
#if defined(AVB0795_HAS_EXT_MAINTENANCE_VARIANT)
      present_fence_info = &present_fence_ext;
#endif
      break;
    case MaintenanceVariant::None:
      break;
    }
    if (present_fence_info == nullptr) {
      vkDestroyFence(device_, present_fence, nullptr);
      observe_performance(PerformanceCounter::FencesDestroyed);
      slot.phase = slot_phase_after_present(PresentOwnership::NotEnqueued);
      set_terminal(VK_ERROR_EXTENSION_NOT_PRESENT, result);
      schedule_poll(result, now);
      return;
    }
    const auto swapchain_handle = swapchain_->handle;
    const VkPresentInfoKHR present_info{
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .pNext = present_fence_info,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &slot.render_finished_semaphore,
      .swapchainCount = 1,
      .pSwapchains = &swapchain_handle,
      .pImageIndices = &image_index,
      .pResults = nullptr,
    };
    present_result = vkQueuePresentKHR(present_queue_, &present_info);
    const auto present = classify_present_result(present_result);
    slot.phase = slot_phase_after_present(present.ownership);
    if (present.ownership == PresentOwnership::NotEnqueued) {
      vkDestroyFence(device_, present_fence, nullptr);
      observe_performance(PerformanceCounter::FencesDestroyed);
      set_terminal(present_result, result);
      schedule_poll(result, now);
      return;
    }

    record.fence = present_fence;
    // Submission completion permits the slot to recycle; presentation owns the
    // consumed semaphore until its separate fence retires the queue operation.
    record.wait_semaphore = slot.render_finished_semaphore;
    record.swapchain = swapchain_handle;
    record.counts_as_presented =
      present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR;
    if (record.counts_as_presented) {
      redraw_requested_ = false;
      // Recording and enqueue run synchronously on the main thread, so this draw
      // contains the latest accepted settings. Subsequent setters mark it dirty
      // again while this independently owned presentation record retires.
      presentation_dirty_ = false;
      record.presentation = slot.recorded_presentation;
      record.presentation->present_sequence = next_present_sequence_++;
    }
    record.confirms_recovery = swapchain_->recovery_candidate;
    record.uncertain = present.ownership == PresentOwnership::Uncertain;
    slot.render_finished_semaphore = VK_NULL_HANDLE;
    slot.acquired_image = false;
    schedule_poll(result, now);

    switch (present.recovery) {
    case PresentRecovery::None:
      break;
    case PresentRecovery::RecreateSwapchain:
      swapchain_recreate_pending_ = true;
      break;
    case PresentRecovery::RecreateSurface:
      if (!surface_recovery_active_) {
        result.surface_recovery_started = true;
      }
      surface_recovery_active_ = true;
      surface_recovery_pending_ = true;
      break;
    case PresentRecovery::RebuildDevice:
      mark_vulkan_device_lost(result);
      break;
    case PresentRecovery::Terminal:
      set_terminal(present_result, result);
      break;
    }
    if (acquire.recreate_after_safe_use) {
      swapchain_recreate_pending_ = true;
    }
  }

  VulkanRenderer::VulkanRenderer(GLFWwindow *window, const std::uint64_t generation, const core::Extent framebuffer, const VulkanStartupRequest &startup, diagnostics::PerformanceReport *performance, const std::uint64_t observation_instance)
      : implementation_{std::make_unique<Impl>(window, generation, framebuffer, startup, performance, observation_instance)} {}
  VulkanRenderer::~VulkanRenderer() noexcept = default;
  VulkanDeviceBorrow VulkanRenderer::device_borrow() const noexcept
  {
    const auto &owner = *implementation_;
    return {.physical_device = owner.physical_device_, .device = owner.device_, .memory_properties = owner.memory_properties_, .limits = owner.physical_properties_.limits, .optional_extension_group_enabled = owner.optional_extension_group_enabled_, .timeline_semaphore_enabled = owner.timeline_semaphore_supported_};
  }
  std::expected<std::uint64_t, core::AppError>
  VulkanRenderer::begin_ring(const core::Extent allocation) noexcept
  { return implementation_->begin_ring(allocation); }
  std::expected<void, core::AppError>
  VulkanRenderer::create_cpu_slot(const SlotKey key)
  {
    auto *slot = implementation_->find_slot(key, RenderSlotPhase::Empty);
    if (slot == nullptr || implementation_->ring_ready_ ||
        slot->upload_buffer != VK_NULL_HANDLE ||
        slot->external_image.image != VK_NULL_HANDLE) {
      return std::unexpected{graphics_error(core::AppErrorCode::InvalidValue)};
    }
    return implementation_->create_capture_slot(*slot);
  }
  std::expected<void, core::AppError>
  VulkanRenderer::register_external_image(const SlotKey key, const ExternalSampledImageView image) noexcept
  { return implementation_->register_external_image(key, image); }
  std::expected<void, core::AppError>
  VulkanRenderer::publish_ring(const std::uint64_t serial, const std::uint64_t generation) noexcept
  { return implementation_->publish_ring(serial, generation); }
  bool VulkanRenderer::ready_for_frame() const noexcept
  { return implementation_->ready_for_frame(); }
  std::optional<SlotKey> VulkanRenderer::reserve_source_slot() noexcept
  { return implementation_->reserve_source_slot(); }
  std::expected<void, core::AppError>
  VulkanRenderer::cancel_source_reservation(const SlotKey key) noexcept
  { return implementation_->cancel_source_reservation(key); }
  std::expected<void, core::AppError>
  VulkanRenderer::complete_cpu_input(const SlotKey key, const CpuRowsView rows, const FrameStamp stamp) noexcept
  { return implementation_->complete_cpu_input(key, rows, stamp); }
  std::expected<void, core::AppError>
  VulkanRenderer::complete_external_input(const SlotKey key, const core::Extent content, const FrameStamp stamp, const CompletedProducerWait wait) noexcept
  { return implementation_->complete_external_input(key, content, stamp, wait); }
  std::expected<void, core::AppError>
  VulkanRenderer::discard_completed_source(const SlotKey key) noexcept
  { return implementation_->discard_completed_source(key); }
  std::expected<void, core::AppError>
  VulkanRenderer::detach_ring(const std::uint64_t serial) noexcept
  { return implementation_->detach_ring(serial); }
  void VulkanRenderer::release_ring_resources() noexcept
  {
    // The facade releases producer imports between detach and this step.
    // A remaining key identifies a missing detach rather than safe teardown.
    for (const auto &slot : implementation_->slots_) {
      if (slot.key.ring_serial != 0) std::terminate();
    }
    implementation_->release_renderer_ring_resources();
  }
  void VulkanRenderer::detach_for_shutdown() noexcept
  { implementation_->detach_for_shutdown(); }
  bool VulkanRenderer::render_allocation_idle() const noexcept
  { return implementation_->render_allocation_idle(); }
  void VulkanRenderer::discard_ready_inputs() noexcept
  { implementation_->discard_ready_inputs(); }
  void VulkanRenderer::set_source_admission(const bool enabled) noexcept
  {
    implementation_->accepting_frames_ = enabled && !implementation_->draining_ &&
                                         !implementation_->terminal_error_ && !implementation_->vulkan_device_lost_;
  }
  void VulkanRenderer::set_presentation(const ReplicaPresentation presentation) noexcept
  { implementation_->set_presentation(presentation); }
  bool VulkanRenderer::has_redraw_work() const noexcept
  { return implementation_->has_redraw_work(); }
  void VulkanRenderer::request_redraw() noexcept
  { implementation_->redraw_requested_ = true; }
  void VulkanRenderer::resize(const core::Extent framebuffer) noexcept
  { implementation_->resize(framebuffer); }
  void VulkanRenderer::begin_drain() noexcept
  { implementation_->begin_drain(); }
  void VulkanRenderer::begin_recovery_drain(const bool device_lost) noexcept
  { implementation_->begin_recovery_drain(device_lost); }
  void VulkanRenderer::note_device_loss() noexcept
  {
    implementation_->vulkan_device_lost_ = true;
    implementation_->accepting_frames_ = false;
  }
  bool VulkanRenderer::vulkan_device_lost() const noexcept
  { return implementation_->vulkan_device_lost_; }
  std::uint64_t VulkanRenderer::generation() const noexcept
  { return implementation_->generation_; }
  std::uint64_t VulkanRenderer::gpu_interop_frames_submitted() const noexcept
  { return implementation_->gpu_interop_frames_submitted_; }
  VulkanTickResult VulkanRenderer::begin_tick(const core::Extent framebuffer,
                                              const std::chrono::steady_clock::time_point now) noexcept
  { return implementation_->begin_tick(framebuffer, now); }
  void VulkanRenderer::retire_submissions(VulkanTickResult &result,
                                          const std::chrono::steady_clock::time_point now) noexcept
  { implementation_->retire_submissions(result, now); }
  void VulkanRenderer::finish_tick(VulkanTickResult &result,
                                   const std::chrono::steady_clock::time_point now,
                                   const bool allow_present,
                                   const bool allow_recovery) noexcept
  { implementation_->finish_tick(result, now, allow_present, allow_recovery); }
  void VulkanRenderer::observe_external_semaphore(const bool created) noexcept
  {
    implementation_->observe_performance(created ? PerformanceCounter::SemaphoresCreated
                                                 : PerformanceCounter::SemaphoresDestroyed);
  }
  void VulkanRenderer::observe_metadata(const VulkanObservationContext context,
                                        const std::chrono::steady_clock::time_point now) noexcept
  { implementation_->observe_metadata(context, now); }

}    // namespace avb::render
