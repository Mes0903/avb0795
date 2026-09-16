/// @file
/// @brief Swapchain resources, presentation retirement and surface recovery.
/// These methods share the renderer's single Impl owner; slot and present proof
/// retain native resources until their existing retirement conditions are met.
#include "render/detail/vulkan_renderer_impl.hpp"

#include "avb/render/replica_shaders.hpp"
#include "render/render_math.hpp"

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace avb::render {
  using diagnostics::PerformanceCounter;
  using diagnostics::PerformanceOwner;
  using diagnostics::PerformanceSample;
  using detail::earlier_deadline;
  using detail::graphics_error;
  using detail::has_zero_dimension;
  using detail::MaintenanceVariant;
  using detail::vulkan_error;

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::create_swapchain(const core::Extent requested)
  {
    if (has_zero_dimension(requested)) {
      return {};
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    auto result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      physical_device_, surface_, &capabilities);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    if ((capabilities.supportedUsageFlags &
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
      return std::unexpected{
        graphics_error(core::AppErrorCode::UnsupportedGraphics)};
    }

    std::uint32_t format_count{};
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_,
                                                  &format_count, nullptr);
    if (result != VK_SUCCESS || format_count == 0) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::UnsupportedGraphics,
        result == VK_SUCCESS ? VK_ERROR_FORMAT_NOT_SUPPORTED : result)};
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_,
                                                  &format_count, formats.data());
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    const auto selected_format = select_sdr_surface_format(formats);
    if (!selected_format) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::UnsupportedGraphics, VK_ERROR_FORMAT_NOT_SUPPORTED)};
    }

    std::uint32_t mode_count{};
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_,
                                                       &mode_count, nullptr);
    if (result != VK_SUCCESS || mode_count == 0) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::UnsupportedGraphics,
        result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result)};
    }
    std::vector<VkPresentModeKHR> modes(mode_count);
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_,
                                                       &mode_count, modes.data());
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }

    VkExtent2D extent{};
    if (capabilities.currentExtent.width !=
        (std::numeric_limits<std::uint32_t>::max)()) {
      extent = capabilities.currentExtent;
    }
    else {
      extent = {
        .width = std::clamp(requested.width, capabilities.minImageExtent.width,
                            capabilities.maxImageExtent.width),
        .height =
          std::clamp(requested.height, capabilities.minImageExtent.height,
                     capabilities.maxImageExtent.height),
      };
    }
    if (extent.width == 0 || extent.height == 0) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                     VK_ERROR_OUT_OF_DATE_KHR)};
    }

    auto image_count = capabilities.minImageCount;
    if (image_count < (std::numeric_limits<std::uint32_t>::max)()) {
      ++image_count;
    }
    if (capabilities.maxImageCount != 0) {
      image_count = (std::min) (image_count, capabilities.maxImageCount);
    }
    const std::array queue_families{graphics_queue_family_,
                                    present_queue_family_};
    const bool separate_queues = graphics_queue_family_ != present_queue_family_;
    constexpr std::array composite_alpha_candidates{
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    const auto composite_alpha = std::ranges::find_if(
      composite_alpha_candidates,
      [&capabilities](const VkCompositeAlphaFlagBitsKHR candidate) noexcept {
        return (capabilities.supportedCompositeAlpha & candidate) != 0;
      });
    if (composite_alpha == composite_alpha_candidates.end()) {
      return std::unexpected{
        graphics_error(core::AppErrorCode::UnsupportedGraphics)};
    }
    const auto active_ownership = swapchain_
                                    ? swapchain_ownership(swapchain_->handle)
                                    : SwapchainOwnershipFacts{};
    const auto create_plan = plan_same_surface_swapchain_create(
      swapchain_.has_value(), active_ownership);
    if (!create_plan.may_create) {
      return std::unexpected{
        graphics_error(core::AppErrorCode::RuntimeStateLost)};
    }
    const auto old_swapchain =
      create_plan.use_active_as_old ? swapchain_->handle : VK_NULL_HANDLE;
    const VkSwapchainCreateInfoKHR create_info{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .pNext = nullptr,
      .flags = 0,
      .surface = surface_,
      .minImageCount = image_count,
      .imageFormat = selected_format->format,
      .imageColorSpace = selected_format->colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = separate_queues ? VK_SHARING_MODE_CONCURRENT
                                          : VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = separate_queues ? 2U : 0U,
      .pQueueFamilyIndices = separate_queues ? queue_families.data() : nullptr,
      .preTransform = capabilities.currentTransform,
      .compositeAlpha = *composite_alpha,
      .presentMode = select_present_mode(modes),
      .clipped = VK_TRUE,
      .oldSwapchain = old_swapchain,
    };

    SwapchainState candidate;
    candidate.format = selected_format->format;
    candidate.extent = extent;
    candidate.present_mode = create_info.presentMode;
    // Passing oldSwapchain retires it even if native creation fails. Move its
    // resources into tracked retirement before the call so failure cannot expose
    // the old handle as active or lose the outstanding present leases.
    if (create_plan.retire_active_before_create) {
      retire_active_swapchain();
    }
    result =
      vkCreateSwapchainKHR(device_, &create_info, nullptr, &candidate.handle);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }

    try {
      auto resources = create_swapchain_render_resources(candidate);
      if (!resources) {
        destroy_swapchain_state(candidate);
        return resources;
      }
    } catch (...) {
      destroy_swapchain_state(candidate);
      throw;
    }
    swapchain_.emplace(std::move(candidate));
    return {};
  }

  std::expected<void, core::AppError>
  VulkanRenderer::Impl::create_swapchain_render_resources(
    SwapchainState &state)
  {
    std::uint32_t image_count{};
    auto result =
      vkGetSwapchainImagesKHR(device_, state.handle, &image_count, nullptr);
    if (result != VK_SUCCESS || image_count == 0) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure,
        result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result)};
    }
    // These image handles belong to WSI. Only the views/framebuffers built around
    // them are destroyed individually when this swapchain record retires.
    state.images.resize(image_count);
    result = vkGetSwapchainImagesKHR(device_, state.handle, &image_count,
                                     state.images.data());
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }

    state.views.reserve(state.images.size());
    for (const auto image : state.images) {
      const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = state.format,
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
      VkImageView view{};
      result = vkCreateImageView(device_, &view_info, nullptr, &view);
      if (result != VK_SUCCESS) {
        return std::unexpected{
          vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
      }
      state.views.push_back(view);
    }

    const VkAttachmentDescription attachment{
      .flags = 0,
      .format = state.format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    const VkAttachmentReference color_reference{
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    const VkSubpassDescription subpass{
      .flags = 0,
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .inputAttachmentCount = 0,
      .pInputAttachments = nullptr,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_reference,
      .pResolveAttachments = nullptr,
      .pDepthStencilAttachment = nullptr,
      .preserveAttachmentCount = 0,
      .pPreserveAttachments = nullptr,
    };
    const VkSubpassDependency dependency{
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dependencyFlags = 0,
    };
    const VkRenderPassCreateInfo render_pass_info{
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 1,
      .pDependencies = &dependency,
    };
    VkRenderPass created_render_pass{VK_NULL_HANDLE};
    result = vkCreateRenderPass(device_, &render_pass_info, nullptr,
                                &created_render_pass);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    state.render_pass = created_render_pass;

    const auto create_shader = [this](const std::span<const std::uint32_t> words,
                                      VkShaderModule &module) noexcept {
      const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = words.size_bytes(),
        .pCode = words.data(),
      };
      VkShaderModule created_module{VK_NULL_HANDLE};
      const auto result =
        vkCreateShaderModule(device_, &info, nullptr, &created_module);
      if (result == VK_SUCCESS) {
        module = created_module;
      }
      return result;
    };
    VkShaderModule vertex{};
    VkShaderModule fragment{};
    result = create_shader(replica_vertex_shader(), vertex);
    if (result == VK_SUCCESS) {
      result = create_shader(replica_fragment_shader(), fragment);
    }
    if (result != VK_SUCCESS) {
      if (vertex != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, vertex, nullptr);
      }
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    const std::array stages{
      VkPipelineShaderStageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vertex,
        .pName = "main",
        .pSpecializationInfo = nullptr,
      },
      VkPipelineShaderStageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fragment,
        .pName = "main",
        .pSpecializationInfo = nullptr,
      }};
    // Geometry is generated from vertex indices. Dynamic viewport/scissor supply
    // aspect fit, so capture-size changes need no vertex allocation or new shader.
    const VkPipelineVertexInputStateCreateInfo vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo input_assembly{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
      .primitiveRestartEnable = VK_FALSE,
    };
    const VkPipelineViewportStateCreateInfo viewport_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .viewportCount = 1,
      .pViewports = nullptr,
      .scissorCount = 1,
      .pScissors = nullptr,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .depthClampEnable = VK_FALSE,
      .rasterizerDiscardEnable = VK_FALSE,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .depthBiasEnable = VK_FALSE,
      .depthBiasConstantFactor = 0.0F,
      .depthBiasClamp = 0.0F,
      .depthBiasSlopeFactor = 0.0F,
      .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
      .sampleShadingEnable = VK_FALSE,
      .minSampleShading = 0.0F,
      .pSampleMask = nullptr,
      .alphaToCoverageEnable = VK_FALSE,
      .alphaToOneEnable = VK_FALSE,
    };
    const VkPipelineColorBlendAttachmentState blend_attachment{
      .blendEnable = VK_FALSE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .logicOpEnable = VK_FALSE,
      .logicOp = VK_LOGIC_OP_COPY,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
      .blendConstants = {},
    };
    constexpr std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
      .pDynamicStates = dynamic_states.data(),
    };
    const VkGraphicsPipelineCreateInfo pipeline_info{
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .stageCount = static_cast<std::uint32_t>(stages.size()),
      .pStages = stages.data(),
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pTessellationState = nullptr,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pDepthStencilState = nullptr,
      .pColorBlendState = &blend,
      .pDynamicState = &dynamic_state,
      .layout = pipeline_layout_,
      .renderPass = state.render_pass,
      .subpass = 0,
      .basePipelineHandle = VK_NULL_HANDLE,
      .basePipelineIndex = -1,
    };
    VkPipeline created_pipeline{VK_NULL_HANDLE};
    result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                       nullptr, &created_pipeline);
    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);
    if (result != VK_SUCCESS) {
      return std::unexpected{
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    state.pipeline = created_pipeline;

    state.framebuffers.reserve(state.views.size());
    for (const auto view : state.views) {
      const VkFramebufferCreateInfo framebuffer_info{
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = state.render_pass,
        .attachmentCount = 1,
        .pAttachments = &view,
        .width = state.extent.width,
        .height = state.extent.height,
        .layers = 1,
      };
      VkFramebuffer framebuffer{};
      result =
        vkCreateFramebuffer(device_, &framebuffer_info, nullptr, &framebuffer);
      if (result != VK_SUCCESS) {
        return std::unexpected{
          vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
      }
      state.framebuffers.push_back(framebuffer);
    }
    return {};
  }

  void VulkanRenderer::Impl::destroy_swapchain_state(
    SwapchainState &state) noexcept
  {
    if (device_ == VK_NULL_HANDLE) {
      state = {};
      return;
    }
    for (const auto framebuffer : state.framebuffers) {
      vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    if (state.pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, state.pipeline, nullptr);
    }
    if (state.render_pass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device_, state.render_pass, nullptr);
    }
    for (const auto view : state.views) {
      vkDestroyImageView(device_, view, nullptr);
    }
    if (state.handle != VK_NULL_HANDLE) {
      vkDestroySwapchainKHR(device_, state.handle, nullptr);
    }
    state = {};
  }

  void VulkanRenderer::Impl::destroy_swapchain() noexcept
  {
    if (!swapchain_) {
      return;
    }
    destroy_swapchain_state(*swapchain_);
    swapchain_.reset();
  }

  void VulkanRenderer::Impl::destroy_retired_swapchains() noexcept
  {
    for (auto &state : retired_swapchains_) {
      destroy_swapchain_state(state);
    }
    retired_swapchains_.clear();
  }

  void VulkanRenderer::Impl::retire_active_swapchain()
  {
    if (!swapchain_) {
      return;
    }
    retired_swapchains_.push_back(std::move(*swapchain_));
    swapchain_.reset();
  }

  SwapchainOwnershipFacts VulkanRenderer::Impl::swapchain_ownership(
    const VkSwapchainKHR handle) const noexcept
  {
    SwapchainOwnershipFacts facts;
    for (const auto &slot : slots_) {
      if (slot.owning_swapchain != handle) {
        continue;
      }
      if (slot.phase == SlotPhase::ReleaseRetained) {
        facts.release_retained = true;
      }
      else if (slot.phase != SlotPhase::Empty) {
        facts.native_image_use_pending = true;
      }
    }
    facts.present_fence_pending = std::ranges::any_of(
      present_records_, [handle](const auto &record) noexcept {
        return record && record->swapchain == handle;
      });
    return facts;
  }

  VulkanRenderer::Impl::PresentRecord *
  VulkanRenderer::Impl::empty_present_record() noexcept
  {
    const auto found = std::ranges::find_if(
      present_records_,
      [](const std::optional<PresentRecord> &record) { return !record; });
    if (found == present_records_.end()) {
      return nullptr;
    }
    return &found->emplace();
  }

  void VulkanRenderer::Impl::destroy_present_records() noexcept
  {
    for (auto &record : present_records_) {
      if (!record) {
        continue;
      }
      if (device_ != VK_NULL_HANDLE) {
        if (record->wait_semaphore != VK_NULL_HANDLE) {
          vkDestroySemaphore(device_, record->wait_semaphore, nullptr);
          observe_performance(PerformanceCounter::SemaphoresDestroyed);
        }
        if (record->fence != VK_NULL_HANDLE) {
          vkDestroyFence(device_, record->fence, nullptr);
          observe_performance(PerformanceCounter::FencesDestroyed);
        }
      }
      record.reset();
    }
  }

  void VulkanRenderer::Impl::resize(const core::Extent framebuffer) noexcept
  {
    if (framebuffer == framebuffer_) {
      return;
    }
    framebuffer_ = framebuffer;
    invalidate_presentation();
    if (!has_zero_dimension(framebuffer_)) {
      swapchain_recreate_pending_ = true;
    }
  }

  void VulkanRenderer::Impl::progress_present_records(
    VulkanTickResult &result, const std::chrono::steady_clock::time_point now)
  {
    if (vulkan_device_lost_) {
      return;
    }
    for (auto &record : present_records_) {
      if (!record) {
        continue;
      }
      const auto status = vkGetFenceStatus(device_, record->fence);
      if (status == VK_NOT_READY) {
        schedule_poll(result, now);
        continue;
      }
      if (status == VK_ERROR_DEVICE_LOST) {
        mark_vulkan_device_lost(result);
        return;
      }
      if (status != VK_SUCCESS) {
        set_terminal(status, result);
        schedule_poll(result, now);
        continue;
      }
      if (record->counts_as_presented) {
        // Enqueue success is insufficient for accounting or retry reset. Only
        // the present fence's retirement discharges this presentation record.
        ++presented_frames_;
        if (performance_) {
          const auto observed = std::chrono::steady_clock::now();
          performance_->observe_runtime({observation_instance_, generation_}, observed);
          performance_->observe_first_present(observed);
          performance_->observe_counter(PerformanceOwner::Replica, PerformanceCounter::PresentRetired, 1, observed);
          if (last_present_observed_at_) {
            performance_->observe_sample(PerformanceSample::PresentRetirementInterval,
              std::chrono::duration_cast<std::chrono::nanoseconds>(observed - *last_present_observed_at_), observed);
          }
          last_present_observed_at_ = observed;
        }
        if (record->confirms_recovery) {
          reset_recovery_after_present(record->swapchain);
        }
        if (record->presentation && swapchain_ &&
            record->swapchain == swapchain_->handle &&
            record->presentation->present_sequence >= first_valid_present_sequence_ &&
            (!published_presentation_ ||
             record->presentation->present_sequence > published_presentation_->present_sequence)) {
          published_presentation_ = record->presentation;
        }
      }
      vkDestroySemaphore(device_, record->wait_semaphore, nullptr);
      observe_performance(PerformanceCounter::SemaphoresDestroyed);
      vkDestroyFence(device_, record->fence, nullptr);
      observe_performance(PerformanceCounter::FencesDestroyed);
      record.reset();
      result.made_progress = true;
    }
  }

  VkResult VulkanRenderer::Impl::release_swapchain_image(
    const VkSwapchainKHR swapchain, const std::uint32_t image_index) noexcept
  {
    switch (maintenance_variant_) {
    case MaintenanceVariant::Khr:
#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
      if (release_images_khr_ != nullptr) {
        const VkReleaseSwapchainImagesInfoKHR release_info{
          .sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_KHR,
          .pNext = nullptr,
          .swapchain = swapchain,
          .imageIndexCount = 1,
          .pImageIndices = &image_index,
        };
        return release_images_khr_(device_, &release_info);
      }
#endif
      break;
    case MaintenanceVariant::Ext:
#if defined(AVB0795_HAS_EXT_MAINTENANCE_VARIANT)
      if (release_images_ext_ != nullptr) {
        const VkReleaseSwapchainImagesInfoEXT release_info{
          .sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT,
          .pNext = nullptr,
          .swapchain = swapchain,
          .imageIndexCount = 1,
          .pImageIndices = &image_index,
        };
        return release_images_ext_(device_, &release_info);
      }
#endif
      break;
    case MaintenanceVariant::None:
      break;
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }

  void VulkanRenderer::Impl::release_acquired_image(
    CaptureSlot &slot, VulkanTickResult &result)
  {
    // progress_submission has already observed the phase's proof fence. Completed
    // but unconsumed binary signals can now be destroyed even when image release
    // fails; that failure still keeps the image attached to its owning swapchain.
    const auto prior_phase = slot.phase;
    const auto release_result =
      release_swapchain_image(slot.owning_swapchain, slot.acquired_image_index);
    const auto classification = classify_release_result(release_result);
    const auto release_plan = plan_slot_release(prior_phase, classification);

    if (release_plan.destroy_acquire_semaphore &&
        slot.acquire_semaphore != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, slot.acquire_semaphore, nullptr);
      observe_performance(PerformanceCounter::SemaphoresDestroyed);
      slot.acquire_semaphore = VK_NULL_HANDLE;
    }
    if (release_plan.destroy_render_finished_semaphore &&
        slot.render_finished_semaphore != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, slot.render_finished_semaphore, nullptr);
      observe_performance(PerformanceCounter::SemaphoresDestroyed);
      slot.render_finished_semaphore = VK_NULL_HANDLE;
    }

    if (release_plan.clear_acquired_ownership) {
      result.made_progress = true;
      reset_slot_after_vulkan(slot);
      return;
    }

    slot.phase = release_plan.next_phase;
    if (release_result == VK_ERROR_DEVICE_LOST) {
      mark_vulkan_device_lost(result);
      return;
    }
    if (classification.recovery == ReleaseRecovery::SurfaceLost) {
      if (!surface_recovery_active_) {
        result.surface_recovery_started = true;
      }
      surface_recovery_active_ = true;
      surface_recovery_pending_ = true;
      return;
    }
    set_terminal(release_result, result);
  }

  void VulkanRenderer::Impl::clear_release_retained_after_swapchain_destroy(
    const VkSwapchainKHR handle)
  {
    for (auto &slot : slots_) {
      if (slot.phase != SlotPhase::ReleaseRetained ||
          slot.owning_swapchain != handle) {
        continue;
      }
      slot.acquired_image = false;
      reset_slot_after_vulkan(slot);
    }
  }

  void VulkanRenderer::Impl::progress_retired_swapchains(
    VulkanTickResult &result)
  {
    auto state = retired_swapchains_.begin();
    while (state != retired_swapchains_.end()) {
      const auto handle = state->handle;
      if (!retired_swapchain_destroyable(swapchain_ownership(handle))) {
        ++state;
        continue;
      }
      destroy_swapchain_state(*state);
      state = retired_swapchains_.erase(state);
      clear_release_retained_after_swapchain_destroy(handle);
      result.made_progress = true;
    }
  }

  void VulkanRenderer::Impl::begin_recovery_episode(
    const GraphicsFailureCategory category,
    const std::chrono::steady_clock::time_point now)
  {
    if (recovery_episode_ && recovery_episode_->category == category) {
      return;
    }
    // A category change starts a new episode. An older present may retire later,
    // but its former candidate marker must not reset this episode's attempt budget.
    if (swapchain_) {
      swapchain_->recovery_candidate = false;
    }
    for (auto &state : retired_swapchains_) {
      state.recovery_candidate = false;
    }
    for (auto &record : present_records_) {
      if (record) {
        record->confirms_recovery = false;
      }
    }
    recovery_episode_ = RecoveryEpisode{
      .category = category,
      .state = {},
      .started_at = now,
    };
    recovery_last_error_.reset();
  }

  void VulkanRenderer::Impl::note_recovery_failure(
    const core::AppError &error, VulkanTickResult &result, const std::chrono::steady_clock::time_point now)
  {
    recovery_last_error_ = error;
    if (error.native_detail == static_cast<std::int64_t>(VK_ERROR_DEVICE_LOST)) {
      mark_vulkan_device_lost(result);
      return;
    }
    if (error.native_detail ==
          static_cast<std::int64_t>(VK_ERROR_SURFACE_LOST_KHR) &&
        (!recovery_episode_ ||
         recovery_episode_->category != GraphicsFailureCategory::Surface)) {
      if (!surface_recovery_active_) {
        result.surface_recovery_started = true;
      }
      surface_recovery_active_ = true;
      surface_recovery_pending_ = true;
      swapchain_recreate_pending_ = false;
      begin_recovery_episode(GraphicsFailureCategory::Surface, now);
      earlier_deadline(
        result.next_deadline,
        now + strictly_future_retry_delay(std::chrono::milliseconds::zero()));
      return;
    }
    if (!recovery_episode_) {
      terminal_error_ = {.code = core::AppErrorCode::RuntimeStateLost,
                         .native_detail = error.native_detail};
      accepting_frames_ = false;
      result.terminal_error = terminal_error_;
      return;
    }

    const GraphicsRetryPolicy policy;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - recovery_episode_->started_at);
    const auto retry =
      policy.evaluate({.category = recovery_episode_->category,
                       .attempt = recovery_episode_->state.attempt,
                       .current_generation = generation_,
                       .elapsed_no_progress = elapsed});
    if (retry.action == GraphicsRetryAction::Terminal || !retry.retry_after) {
      terminal_error_ = {.code = core::AppErrorCode::RuntimeStateLost,
                         .native_detail = error.native_detail};
      accepting_frames_ = false;
      result.terminal_error = terminal_error_;
      return;
    }
    earlier_deadline(result.next_deadline,
                     now + strictly_future_retry_delay(*retry.retry_after));
  }

  void VulkanRenderer::Impl::reset_recovery_after_present(
    const VkSwapchainKHR handle) noexcept
  {
    if (!recovery_episode_) {
      return;
    }
    const auto is_candidate =
      (swapchain_ && swapchain_->handle == handle &&
       swapchain_->recovery_candidate) ||
      std::ranges::any_of(
        retired_swapchains_, [handle](const SwapchainState &state) noexcept {
          return state.handle == handle && state.recovery_candidate;
        });
    if (!is_candidate) {
      return;
    }
    recovery_episode_->state = advance_graphics_recovery_episode(
      recovery_episode_->state,
      GraphicsRecoveryObservation::SuccessfulPresentRetired);
    recovery_episode_.reset();
    recovery_last_error_.reset();
    if (swapchain_) {
      swapchain_->recovery_candidate = false;
    }
    for (auto &state : retired_swapchains_) {
      state.recovery_candidate = false;
    }
    for (auto &record : present_records_) {
      if (record) {
        record->confirms_recovery = false;
      }
    }
  }

  void VulkanRenderer::Impl::progress_recreation(
    VulkanTickResult &result, const std::chrono::steady_clock::time_point now)
  {
    progress_retired_swapchains(result);

    if (draining_) {
      if (swapchain_) {
        const auto ownership = swapchain_ownership(swapchain_->handle);
        if (!ownership.native_image_use_pending) {
          retire_active_swapchain();
          result.made_progress = true;
          progress_retired_swapchains(result);
        }
      }
      if (surface_recovery_pending_ && !swapchain_ &&
          retired_swapchains_.empty() && surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        surface_recovery_pending_ = false;
        result.made_progress = true;
      }
      return;
    }
    if (terminal_error_ || vulkan_device_lost_ ||
        has_zero_dimension(framebuffer_)) {
      return;
    }

    const auto category =
      surface_recovery_pending_ ||
          (recovery_episode_ &&
           recovery_episode_->category ==
             GraphicsFailureCategory::Surface &&
           recovery_episode_->state.awaiting_successful_present)
        ? GraphicsFailureCategory::Surface
        : GraphicsFailureCategory::Swapchain;
    const bool recreate_surface = category == GraphicsFailureCategory::Surface;
    if (!surface_recovery_pending_ && !swapchain_recreate_pending_ &&
        swapchain_) {
      return;
    }
    if (recreate_surface && !surface_recovery_pending_) {
      if (!surface_recovery_active_) {
        result.surface_recovery_started = true;
      }
      surface_recovery_active_ = true;
      surface_recovery_pending_ = true;
      swapchain_recreate_pending_ = false;
    }
    begin_recovery_episode(category, now);

    if (recreate_surface) {
      if (swapchain_) {
        const auto ownership = swapchain_ownership(swapchain_->handle);
        if (ownership.native_image_use_pending) {
          schedule_poll(result, now);
          return;
        }
        retire_active_swapchain();
        result.made_progress = true;
      }
      progress_retired_swapchains(result);
      if (!retired_swapchains_.empty()) {
        // A surface cannot be replaced while any retired child still owes native
        // image-use or present proof, even if a new swapchain could otherwise fit.
        schedule_poll(result, now);
        return;
      }
    }
    else {
      const auto active_ownership = swapchain_
                                      ? swapchain_ownership(swapchain_->handle)
                                      : SwapchainOwnershipFacts{};
      const auto create_plan = plan_same_surface_swapchain_create(
        swapchain_.has_value(), active_ownership);
      if (!create_plan.may_create) {
        schedule_poll(result, now);
        return;
      }
    }

    const GraphicsRetryPolicy policy;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - recovery_episode_->started_at);
    const auto retry =
      policy.evaluate({.category = category,
                       .attempt = recovery_episode_->state.attempt,
                       .current_generation = generation_,
                       .elapsed_no_progress = elapsed});
    if (retry.action == GraphicsRetryAction::Terminal || !retry.retry_after) {
      const auto detail = recovery_last_error_
                            ? recovery_last_error_->native_detail
                            : std::optional<std::int64_t>{};
      terminal_error_ = {.code = core::AppErrorCode::RuntimeStateLost,
                         .native_detail = detail};
      accepting_frames_ = false;
      result.terminal_error = terminal_error_;
      return;
    }
    if (*retry.retry_after > std::chrono::milliseconds::zero()) {
      earlier_deadline(result.next_deadline, now + *retry.retry_after);
      return;
    }

    recovery_episode_->state = advance_graphics_recovery_episode(
      recovery_episode_->state,
      GraphicsRecoveryObservation::ConstructionAttempted);

    if (recreate_surface) {
      if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
      }
      try {
        create_surface();
      } catch (const core::AppError &error) {
        note_recovery_failure(error, result, now);
        return;
      }
    }

    const auto active_before_create =
      swapchain_ ? swapchain_->handle : VK_NULL_HANDLE;
    const auto created = create_swapchain(framebuffer_);
    const bool active_was_retired =
      active_before_create != VK_NULL_HANDLE &&
      (!swapchain_ || swapchain_->handle != active_before_create);
    result.made_progress = result.made_progress || active_was_retired;
    if (!created) {
      note_recovery_failure(created.error(), result, now);
      progress_retired_swapchains(result);
      return;
    }
    if (!swapchain_) {
      note_recovery_failure(
        vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                     VK_ERROR_OUT_OF_DATE_KHR),
        result, now);
      return;
    }

    // Construction restores a candidate, not the retry budget. Retained content
    // requests one redraw so recovery can be confirmed without a new capture.
    recovery_episode_->state = advance_graphics_recovery_episode(
      recovery_episode_->state,
      GraphicsRecoveryObservation::ReplacementConstructed);
    swapchain_->recovery_candidate = true;
    redraw_requested_ = true;
    recovery_last_error_.reset();
    swapchain_recreate_pending_ = false;

    if (recreate_surface) {
      surface_recovery_pending_ = false;
      surface_recovery_active_ = false;
      result.surface_recovery_completed = true;
    }
    progress_retired_swapchains(result);
  }

}    // namespace avb::render
