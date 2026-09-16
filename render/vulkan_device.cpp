/// @file
/// @brief Vulkan bootstrap and device-scoped resource methods of the replica owner.
/// The renderer coordinates aggregate construction and teardown; these methods
/// establish device capabilities, queues and common resources in that sequence.
#include "render/detail/vulkan_renderer_impl.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace avb::render {
  using diagnostics::PerformanceCounter;
  using detail::cpu_slot_count;
  using detail::graphics_error;
  using detail::MaintenanceVariant;
  using detail::ReplicaPushConstants;
  using detail::vulkan_error;

  namespace {
    [[nodiscard]] bool
    extension_available(const std::span<const VkExtensionProperties> extensions,
                        const char *const required) noexcept
    {
      return std::ranges::any_of(
        extensions, [required](const VkExtensionProperties &extension) noexcept {
          return std::strcmp(extension.extensionName, required) == 0;
        });
    }

    // Preferences cannot make an incompatible memory type usable. Keep the first
    // required-only match as a fallback when no compatible preferred type exists.
    [[nodiscard]] std::optional<std::uint32_t>
    memory_type_index(const VkPhysicalDeviceMemoryProperties &properties,
                      std::uint32_t bits,
                      VkMemoryPropertyFlags required,
                      VkMemoryPropertyFlags preferred = 0) noexcept
    {
      std::optional<std::uint32_t> required_only;
      for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((bits & (std::uint32_t{1} << index)) == 0) {
          continue;
        }
        const auto flags = properties.memoryTypes[index].propertyFlags;
        if ((flags & required) != required) {
          continue;
        }
        if ((flags & preferred) == preferred) {
          return index;
        }
        if (!required_only) {
          required_only = index;
        }
      }
      return required_only;
    }

  }    // namespace

  void VulkanRenderer::Impl::create_vulkan_instance()
  {
    if (glfwVulkanSupported() != GLFW_TRUE) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }

    std::uint32_t available_count{};
    auto result = vkEnumerateInstanceExtensionProperties(
      nullptr, &available_count, nullptr);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result);
    }
    std::vector<VkExtensionProperties> available_extensions(available_count);
    result = vkEnumerateInstanceExtensionProperties(nullptr, &available_count,
                                                    available_extensions.data());
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result);
    }

    std::uint32_t required_count{};
    const auto *const required =
      glfwGetRequiredInstanceExtensions(&required_count);
    if (required == nullptr || required_count == 0) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }
    std::vector<const char *> enabled_extensions;
    enabled_extensions.reserve(static_cast<std::size_t>(required_count) + 3);
    const auto append_unique = [&enabled_extensions](const char *const name) {
      if (std::ranges::none_of(enabled_extensions,
                               [name](const char *const enabled) noexcept {
                                 return std::strcmp(enabled, name) == 0;
                               })) {
        enabled_extensions.push_back(name);
      }
    };
    for (std::uint32_t index{}; index < required_count; ++index) {
      if (!extension_available(available_extensions, required[index])) {
        throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
      }
      append_unique(required[index]);
    }

#if defined(VK_KHR_get_surface_capabilities2)
    const auto capabilities2_available = extension_available(
      available_extensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
#else
    constexpr auto capabilities2_available = false;
#endif
#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
    khr_surface_maintenance_enabled_ =
      capabilities2_available &&
      extension_available(available_extensions,
                          VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
#endif
#if defined(AVB0795_HAS_EXT_MAINTENANCE_VARIANT)
    ext_surface_maintenance_enabled_ =
      capabilities2_available &&
      extension_available(available_extensions,
                          VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
#endif
    // Both transports depend on present fences and explicit unused-image release.
    // Falling back to CPU copies therefore cannot relax this WSI requirement.
    if (!khr_surface_maintenance_enabled_ && !ext_surface_maintenance_enabled_) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }
#if defined(VK_KHR_get_surface_capabilities2)
    append_unique(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
#endif
#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
    if (khr_surface_maintenance_enabled_) {
      append_unique(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    }
#endif
#if defined(AVB0795_HAS_EXT_MAINTENANCE_VARIANT)
    if (ext_surface_maintenance_enabled_) {
      append_unique(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    }
#endif

    const VkApplicationInfo application{
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pNext = nullptr,
      .pApplicationName = "avb0795",
      .applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
      .pEngineName = "avb0795",
      .engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
      .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .pApplicationInfo = &application,
      .enabledLayerCount = 0,
      .ppEnabledLayerNames = nullptr,
      .enabledExtensionCount =
        static_cast<std::uint32_t>(enabled_extensions.size()),
      .ppEnabledExtensionNames = enabled_extensions.data(),
    };
    VkInstance created_instance{VK_NULL_HANDLE};
    result = vkCreateInstance(&create_info, nullptr, &created_instance);
    if (result != VK_SUCCESS) {
      throw vulkan_error(vulkan_instance_create_error_code(result), result);
    }
    instance_ = created_instance;
  }

  void VulkanRenderer::Impl::create_surface()
  {
    const auto result =
      glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result);
    }
  }

  void VulkanRenderer::Impl::select_physical_device(
    const VulkanStartupRequest &request)
  {
    std::uint32_t device_count{};
    auto result = vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (result != VK_SUCCESS || device_count == 0) {
      throw vulkan_error(core::AppErrorCode::UnsupportedGraphics,
                         result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED
                                              : result);
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    result = vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::UnsupportedGraphics, result);
    }

    // Stable ranking preserves equally preferred candidates; every candidate
    // still passes the complete Vulkan/WSI floor below.
    if (request.prefer_candidate) {
      std::ranges::stable_sort(
        devices, [&request](const VkPhysicalDevice left, const VkPhysicalDevice right) {
          return request.prefer_candidate(left) && !request.prefer_candidate(right);
        });
    }

    for (const auto candidate : devices) {
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(candidate, &properties);
      if (properties.apiVersion < VK_API_VERSION_1_3) {
        continue;
      }

      std::uint32_t extension_count{};
      result = vkEnumerateDeviceExtensionProperties(candidate, nullptr,
                                                    &extension_count, nullptr);
      if (result != VK_SUCCESS) {
        continue;
      }
      std::vector<VkExtensionProperties> extensions(extension_count);
      result = vkEnumerateDeviceExtensionProperties(
        candidate, nullptr, &extension_count, extensions.data());
      if (result != VK_SUCCESS ||
          !extension_available(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        continue;
      }

      auto candidate_variant = MaintenanceVariant::None;
#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
      if (khr_surface_maintenance_enabled_ &&
          extension_available(extensions,
                              VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance{
          .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
          .pNext = nullptr,
          .swapchainMaintenance1 = VK_FALSE,
        };
        VkPhysicalDeviceFeatures2 features{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
          .pNext = &maintenance,
        };
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (maintenance.swapchainMaintenance1 == VK_TRUE) {
          candidate_variant = MaintenanceVariant::Khr;
        }
      }
#endif
#if defined(AVB0795_HAS_EXT_MAINTENANCE_VARIANT)
      if (candidate_variant == MaintenanceVariant::None &&
          ext_surface_maintenance_enabled_ &&
          extension_available(extensions,
                              VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
          .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
          .pNext = nullptr,
          .swapchainMaintenance1 = VK_FALSE,
        };
        VkPhysicalDeviceFeatures2 features{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
          .pNext = &maintenance,
        };
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (maintenance.swapchainMaintenance1 == VK_TRUE) {
          candidate_variant = MaintenanceVariant::Ext;
        }
      }
#endif
      if (candidate_variant == MaintenanceVariant::None) {
        continue;
      }
      VkFormatProperties capture_format_properties{};
      vkGetPhysicalDeviceFormatProperties(candidate, VK_FORMAT_B8G8R8A8_UNORM,
                                          &capture_format_properties);
      constexpr auto required_capture_features =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
      if ((capture_format_properties.optimalTilingFeatures &
           required_capture_features) != required_capture_features) {
        continue;
      }

      std::uint32_t queue_count{};
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
      std::vector<VkQueueFamilyProperties> queues(queue_count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count,
                                               queues.data());
      std::optional<std::uint32_t> graphics_family;
      std::optional<std::uint32_t> present_family;
      for (std::uint32_t index = 0; index < queue_count; ++index) {
        if (!graphics_family &&
            (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
          graphics_family = index;
        }
        VkBool32 supports_present{};
        if (vkGetPhysicalDeviceSurfaceSupportKHR(
              candidate, index, surface_, &supports_present) == VK_SUCCESS &&
            supports_present == VK_TRUE && !present_family) {
          present_family = index;
        }
      }
      if (!graphics_family || !present_family) {
        continue;
      }

      std::uint32_t format_count{};
      std::uint32_t mode_count{};
      if (vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface_, &format_count,
                                               nullptr) != VK_SUCCESS ||
          vkGetPhysicalDeviceSurfacePresentModesKHR(
            candidate, surface_, &mode_count, nullptr) != VK_SUCCESS ||
          format_count == 0 || mode_count == 0) {
        continue;
      }

      physical_device_ = candidate;
      maintenance_variant_ = candidate_variant;
      physical_properties_ = properties;
      graphics_queue_family_ = *graphics_family;
      present_queue_family_ = *present_family;
      vkGetPhysicalDeviceMemoryProperties(candidate, &memory_properties_);
      optional_extension_group_enabled_ =
        !request.optional_device_extension_group.empty() &&
        std::ranges::all_of(request.optional_device_extension_group,
                            [&extensions](const char *name) noexcept {
          return extension_available(extensions, name);
        });
      VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .pNext = nullptr,
        .timelineSemaphore = VK_FALSE,
      };
      VkPhysicalDeviceFeatures2 optional_features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &timeline_features,
      };
      vkGetPhysicalDeviceFeatures2(candidate, &optional_features);
      timeline_semaphore_supported_ =
        request.request_timeline_semaphore &&
        timeline_features.timelineSemaphore == VK_TRUE;
      return;
    }
    throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
  }

  void VulkanRenderer::Impl::create_vulkan_device(
    const VulkanStartupRequest &request)
  {
    constexpr float priority = 1.0F;
    std::array<VkDeviceQueueCreateInfo, 2> queue_infos{};
    std::uint32_t queue_info_count{1};
    queue_infos[0] = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .queueFamilyIndex = graphics_queue_family_,
      .queueCount = 1,
      .pQueuePriorities = &priority,
    };
    if (present_queue_family_ != graphics_queue_family_) {
      queue_infos[1] = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = present_queue_family_,
        .queueCount = 1,
        .pQueuePriorities = &priority,
      };
      queue_info_count = 2;
    }

#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance_khr{
      .sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
      .pNext = nullptr,
      .swapchainMaintenance1 = VK_TRUE,
    };
#endif
#if defined(AVB0795_HAS_EXT_MAINTENANCE_VARIANT)
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance_ext{
      .sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
      .pNext = nullptr,
      .swapchainMaintenance1 = VK_TRUE,
    };
#endif
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
      .pNext = nullptr,
      .timelineSemaphore = timeline_semaphore_supported_ ? VK_TRUE : VK_FALSE,
    };
    const void *maintenance_features{};
    const char *maintenance_extension{};
    switch (maintenance_variant_) {
    case MaintenanceVariant::Khr:
#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
      maintenance_features = &maintenance_khr;
      maintenance_extension = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
#endif
      break;
    case MaintenanceVariant::Ext:
#if defined(AVB0795_HAS_EXT_MAINTENANCE_VARIANT)
      maintenance_features = &maintenance_ext;
      maintenance_extension = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
#endif
      break;
    case MaintenanceVariant::None:
      break;
    }
    if (maintenance_features == nullptr || maintenance_extension == nullptr) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }
    if (timeline_semaphore_supported_) {
      switch (maintenance_variant_) {
      case MaintenanceVariant::Khr:
#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
        maintenance_khr.pNext = &timeline_features;
#endif
        break;
      case MaintenanceVariant::Ext:
#if defined(AVB0795_HAS_EXT_MAINTENANCE_VARIANT)
        maintenance_ext.pNext = &timeline_features;
#endif
        break;
      case MaintenanceVariant::None:
        break;
      }
    }
    std::vector<const char *> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                         maintenance_extension};
    if (optional_extension_group_enabled_) {
      extensions.insert(extensions.end(), request.optional_device_extension_group.begin(),
                        request.optional_device_extension_group.end());
    }
    const VkDeviceCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = maintenance_features,
      .flags = 0,
      .queueCreateInfoCount = queue_info_count,
      .pQueueCreateInfos = queue_infos.data(),
      .enabledLayerCount = 0,
      .ppEnabledLayerNames = nullptr,
      .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
      .ppEnabledExtensionNames = extensions.data(),
      .pEnabledFeatures = nullptr,
    };
    VkDevice created_device{VK_NULL_HANDLE};
    const auto result =
      vkCreateDevice(physical_device_, &create_info, nullptr, &created_device);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::UnsupportedGraphics, result);
    }
    device_ = created_device;
    vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, present_queue_family_, 0, &present_queue_);
    auto release_function_loaded = false;
    switch (maintenance_variant_) {
    case MaintenanceVariant::Khr:
#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
      release_images_khr_ = reinterpret_cast<PFN_vkReleaseSwapchainImagesKHR>(
        vkGetDeviceProcAddr(device_, "vkReleaseSwapchainImagesKHR"));
      release_function_loaded = release_images_khr_ != nullptr;
#endif
      break;
    case MaintenanceVariant::Ext:
#if defined(AVB0795_HAS_EXT_MAINTENANCE_VARIANT)
      release_images_ext_ = reinterpret_cast<PFN_vkReleaseSwapchainImagesEXT>(
        vkGetDeviceProcAddr(device_, "vkReleaseSwapchainImagesEXT"));
      release_function_loaded = release_images_ext_ != nullptr;
#endif
      break;
    case MaintenanceVariant::None:
      break;
    }
    if (!release_function_loaded) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }
  }

  void VulkanRenderer::Impl::create_vulkan_common_resources()
  {
    const VkDescriptorSetLayoutBinding binding{
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .pImmutableSamplers = nullptr,
    };
    const VkDescriptorSetLayoutCreateInfo descriptor_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .bindingCount = 1,
      .pBindings = &binding,
    };
    VkDescriptorSetLayout created_descriptor_layout{VK_NULL_HANDLE};
    auto result = vkCreateDescriptorSetLayout(
      device_, &descriptor_info, nullptr, &created_descriptor_layout);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result);
    }
    descriptor_layout_ = created_descriptor_layout;

    // Fragment sampling and the selection outline share four vec4 blocks.
    // Both capture transports use the same allocation-normalized interface.
    const VkPushConstantRange push_constant{
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = sizeof(ReplicaPushConstants),
    };
    const VkPipelineLayoutCreateInfo pipeline_layout_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .setLayoutCount = 1,
      .pSetLayouts = &descriptor_layout_,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_constant,
    };
    VkPipelineLayout created_pipeline_layout{VK_NULL_HANDLE};
    result = vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
                                    &created_pipeline_layout);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result);
    }
    pipeline_layout_ = created_pipeline_layout;

    const VkDescriptorPoolSize pool_size{
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = static_cast<std::uint32_t>(cpu_slot_count),
    };
    const VkDescriptorPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .maxSets = static_cast<std::uint32_t>(cpu_slot_count),
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
    };
    VkDescriptorPool created_descriptor_pool{VK_NULL_HANDLE};
    result = vkCreateDescriptorPool(device_, &pool_info, nullptr,
                                    &created_descriptor_pool);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result);
    }
    descriptor_pool_ = created_descriptor_pool;

    const VkCommandPoolCreateInfo command_pool_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .pNext = nullptr,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = graphics_queue_family_,
    };
    VkCommandPool created_command_pool{VK_NULL_HANDLE};
    result = vkCreateCommandPool(device_, &command_pool_info, nullptr,
                                 &created_command_pool);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result);
    }
    command_pool_ = created_command_pool;

    std::array<VkCommandBuffer, cpu_slot_count> command_buffers{};
    const VkCommandBufferAllocateInfo command_allocate_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .pNext = nullptr,
      .commandPool = command_pool_,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = static_cast<std::uint32_t>(command_buffers.size()),
    };
    result = vkAllocateCommandBuffers(device_, &command_allocate_info,
                                      command_buffers.data());
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result);
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      slots_[index].command_buffer = command_buffers[index];
    }
    observe_performance(PerformanceCounter::CommandBuffersCreated, command_buffers.size());

    const VkSamplerCreateInfo sampler_info{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .mipLodBias = 0.0F,
      .anisotropyEnable = VK_FALSE,
      .maxAnisotropy = 1.0F,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .minLod = 0.0F,
      .maxLod = 0.0F,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
    };
    VkSampler created_sampler{VK_NULL_HANDLE};
    result = vkCreateSampler(device_, &sampler_info, nullptr, &created_sampler);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result);
    }
    sampler_ = created_sampler;
  }

  void VulkanRenderer::Impl::destroy_vulkan_common_resources() noexcept
  {
    if (device_ == VK_NULL_HANDLE) {
      return;
    }
    if (sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, sampler_, nullptr);
      sampler_ = VK_NULL_HANDLE;
    }
    if (command_pool_ != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device_, command_pool_, nullptr);
      observe_performance(PerformanceCounter::CommandBuffersDestroyed, performance_live_.command_buffers);
      command_pool_ = VK_NULL_HANDLE;
      for (auto &slot : slots_) {
        slot.command_buffer = VK_NULL_HANDLE;
      }
    }
    destroy_descriptor_pool();
    if (pipeline_layout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
      pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (descriptor_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
      descriptor_layout_ = VK_NULL_HANDLE;
    }
  }

  std::optional<std::uint32_t> VulkanRenderer::Impl::find_memory_type(
    const std::uint32_t bits, const VkMemoryPropertyFlags required, const VkMemoryPropertyFlags preferred) const noexcept
  {
    return memory_type_index(memory_properties_, bits, required, preferred);
  }

  void VulkanRenderer::Impl::destroy_descriptor_pool() noexcept
  {
    if (device_ != VK_NULL_HANDLE && descriptor_pool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
      descriptor_pool_ = VK_NULL_HANDLE;
    }
  }

}    // namespace avb::render
