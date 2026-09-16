/// @file
/// @brief D3 producer proof and Windows external imports using a borrowed Vulkan device.
#include "platform/windows/windows_frame_bridge.hpp"

#include "platform/windows/detail/windows_graphics_policy.hpp"
#include "platform/windows/windows_captured_frame.hpp"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace avb::platform::windows {
  namespace {
    using Microsoft::WRL::ComPtr;

    constexpr std::size_t slot_count = 3;
    constexpr std::uint32_t bytes_per_pixel = 4;
    constexpr auto pending_poll_interval = std::chrono::milliseconds{2};

    [[nodiscard]] constexpr core::AppError bridge_error(
      const core::AppErrorCode code,
      const std::optional<std::int64_t> detail = std::nullopt) noexcept
    {
      return {.code = code, .native_detail = detail};
    }

    [[nodiscard]] constexpr core::AppError
    d3_error(const core::AppErrorCode code, const HRESULT result) noexcept
    {
      return bridge_error(code, static_cast<std::int64_t>(result));
    }

    [[nodiscard]] constexpr core::AppError
    vulkan_error(const core::AppErrorCode code, const VkResult result) noexcept
    {
      return bridge_error(code, static_cast<std::int64_t>(result));
    }

    /// @brief Move-only owner of a temporary NT sharing handle; importing native APIs
    /// retain their own resource references, so the handle can close afterward.
    class UniqueWin32Handle final {
    public:
      UniqueWin32Handle() noexcept = default;
      explicit UniqueWin32Handle(HANDLE handle) noexcept : handle_{handle} {}
      ~UniqueWin32Handle() noexcept { reset(); }

      UniqueWin32Handle(const UniqueWin32Handle &) = delete;
      UniqueWin32Handle &operator=(const UniqueWin32Handle &) = delete;

      UniqueWin32Handle(UniqueWin32Handle &&other) noexcept
          : handle_{std::exchange(other.handle_, nullptr)} {}
      UniqueWin32Handle &operator=(UniqueWin32Handle &&other) noexcept
      {
        if (this != &other) {
          reset();
          handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
      }

      [[nodiscard]] HANDLE get() const noexcept { return handle_; }
      [[nodiscard]] explicit operator bool() const noexcept
      {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
      }

      // Clearing the wrapper makes repeated reset and moved-from destruction safe.
      void reset() noexcept
      {
        if (*this) {
          static_cast<void>(CloseHandle(handle_));
        }
        handle_ = nullptr;
      }

    private:
      HANDLE handle_{};
    };

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


    // Validate the same allocation-sized byte count required by the CPU upload
    // owner before either side starts allocating or addressing source rows.
    [[nodiscard]] bool valid_allocation(const core::Extent extent) noexcept
    {
      if (extent.width == 0 || extent.height == 0) {
        return false;
      }
      constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
      const auto width = static_cast<std::uint64_t>(extent.width);
      const auto height = static_cast<std::uint64_t>(extent.height);
      if (width > maximum / height || width * height > maximum / bytes_per_pixel) {
        return false;
      }
      return width * height * bytes_per_pixel <=
             (std::numeric_limits<std::size_t>::max)();
    }

    /// @brief Unmap one successful native Map while its slot and context remain owned.
    class MappedReadback final {
    public:
      MappedReadback(ID3D11DeviceContext &context, ID3D11Texture2D &texture) noexcept
          : context_{context}, texture_{texture} {}
      ~MappedReadback() noexcept { context_.Unmap(&texture_, 0); }
      MappedReadback(const MappedReadback &) = delete;
      MappedReadback &operator=(const MappedReadback &) = delete;

    private:
      ID3D11DeviceContext &context_;
      ID3D11Texture2D &texture_;
    };
  }    // namespace

  class WindowsFrameBridge::Impl final {
  public:
    /// Each value names actual producer proof, never renderer readiness or use.
    enum class CopyProof { QueryPending, FencePending, CpuMapPending };

    struct PendingCopy final {
      std::optional<WindowsCapturedFrame> frame;
      core::Extent content{};
      render::FrameStamp stamp;
      CopyProof proof{CopyProof::QueryPending};
      // Slot and shared-support owners outlive this obligation, including when
      // the renderer independently loses its Vulkan device.
      bool external_copy{};
      std::uint64_t target{};
    };

    struct ProducerSlot final {
      render::SlotKey key;
      ComPtr<ID3D11Texture2D> staging;
      ComPtr<ID3D11Query> query;
      ComPtr<ID3D11Texture2D> interop_texture;
      UniqueWin32Handle interop_handle;
      VkImage interop_image{VK_NULL_HANDLE};
      VkDeviceMemory interop_memory{VK_NULL_HANDLE};
      VkImageView interop_view{VK_NULL_HANDLE};
      VkMemoryRequirements interop_memory_requirements{};
      std::uint32_t interop_memory_type_index{};
      bool interop_requires_dedicated{};
      bool interop_compatible{};
      // Remains engaged after query proof releases frame when Map must retry.
      std::optional<PendingCopy> pending;
    };

    explicit Impl(const BridgeResourceObserver observer) : observer_{observer}
    {
      constexpr std::array requested_levels{D3D_FEATURE_LEVEL_11_1,
                                            D3D_FEATURE_LEVEL_11_0};
      D3D_FEATURE_LEVEL selected_level{};
      const auto result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested_levels.data(),
        static_cast<UINT>(requested_levels.size()), D3D11_SDK_VERSION,
        device_.GetAddressOf(), &selected_level, context_.GetAddressOf());
      if (FAILED(result)) {
        throw d3_error(core::AppErrorCode::UnsupportedGraphics, result);
      }
      static_cast<void>(selected_level);
    }

    ~Impl() noexcept
    {
      // Even a latched removal is delivered as keyed events before destruction;
      // the caller must not abandon its canonical reservations during unwind.
      if (has_pending_inputs()) {
        std::terminate();
      }
      for (auto &slot : slots_) {
        destroy_interop_slot(slot);
      }
      release_interop_support();
    }

    [[nodiscard]] ID3D11Device &capture_device() noexcept { return *device_.Get(); }

    [[nodiscard]] render::VulkanStartupRequest startup_request() const
    {
      ComPtr<IDXGIDevice> dxgi_device;
      ComPtr<IDXGIAdapter> adapter;
      DXGI_ADAPTER_DESC description{};
      const bool available =
        SUCCEEDED(device_.As(&dxgi_device)) &&
        SUCCEEDED(dxgi_device->GetAdapter(adapter.GetAddressOf())) &&
        SUCCEEDED(adapter->GetDesc(&description));
      static constexpr std::array extensions{
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
      return {
        .prefer_candidate = [available, luid = description.AdapterLuid](
                              const VkPhysicalDevice candidate) noexcept {
          if (!available) {
            return false;
          }
          VkPhysicalDeviceIDProperties id{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
          VkPhysicalDeviceProperties2 properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &id};
          vkGetPhysicalDeviceProperties2(candidate, &properties);
          static_assert(sizeof(LUID) == VK_LUID_SIZE);
          return id.deviceLUIDValid == VK_TRUE &&
                 std::memcmp(&luid, id.deviceLUID, VK_LUID_SIZE) == 0;
        },
        .optional_device_extension_group = extensions,
        .request_timeline_semaphore = true,
      };
    }

    void attach_vulkan_device(const render::VulkanDeviceBorrow device) noexcept
    {
      if (vulkan_.device != VK_NULL_HANDLE || device.device == VK_NULL_HANDLE ||
          device.physical_device == VK_NULL_HANDLE || ring_serial_ != 0) {
        std::terminate();
      }
      vulkan_ = device;
      if (vulkan_.optional_extension_group_enabled) {
        get_memory_handle_properties_ =
          reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
            vkGetDeviceProcAddr(vulkan_.device, "vkGetMemoryWin32HandlePropertiesKHR"));
        import_semaphore_handle_ =
          reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(vulkan_.device, "vkImportSemaphoreWin32HandleKHR"));
      }
      refresh_interop_capabilities();
    }

    [[nodiscard]] render::BridgeCapabilityFacts capability_facts() const noexcept
    {
      return bridge_facts_;
    }

    [[nodiscard]] bool d3_device_removed() const noexcept
    {
      return d3_device_removed_ || FAILED(device_->GetDeviceRemovedReason());
    }

    void refresh_interop_capabilities() noexcept
    {
      // These queries establish a device-level prefix. Memory compatibility stays
      // unproven until slot creation queries the handles for the actual allocation.
      if (ring_serial_ != 0 || interop_timeline_ != VK_NULL_HANDLE ||
          vulkan_.device == VK_NULL_HANDLE) {
        std::terminate();
      }
      bridge_facts_ = {};
      device5_.Reset();
      context4_.Reset();
      interop_fence_.Reset();
      try {
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapter_description{};
        const auto has_d3_luid =
          SUCCEEDED(capture_device().QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
          SUCCEEDED(dxgi_device->GetAdapter(adapter.GetAddressOf())) &&
          SUCCEEDED(adapter->GetDesc(&adapter_description));

        VkPhysicalDeviceIDProperties id_properties{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
          .pNext = nullptr,
        };
        VkPhysicalDeviceProperties2 properties{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
          .pNext = &id_properties,
        };
        vkGetPhysicalDeviceProperties2(vulkan_.physical_device, &properties);
        static_assert(sizeof(LUID) == VK_LUID_SIZE);
        bridge_facts_.adapter_luid_matches =
          has_d3_luid && id_properties.deviceLUIDValid == VK_TRUE &&
          std::memcmp(&adapter_description.AdapterLuid, id_properties.deviceLUID,
                      VK_LUID_SIZE) == 0;

        const VkPhysicalDeviceExternalImageFormatInfo external_image{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
          .pNext = nullptr,
          .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
        };
        const VkPhysicalDeviceImageFormatInfo2 image_info{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
          .pNext = &external_image,
          .format = VK_FORMAT_B8G8R8A8_UNORM,
          .type = VK_IMAGE_TYPE_2D,
          .tiling = VK_IMAGE_TILING_OPTIMAL,
          .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
          .flags = 0,
        };
        VkExternalImageFormatProperties external_properties{
          .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
          .pNext = nullptr,
        };
        VkImageFormatProperties2 image_properties{
          .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
          .pNext = &external_properties,
        };
        const auto image_result = vkGetPhysicalDeviceImageFormatProperties2(
          vulkan_.physical_device, &image_info, &image_properties);
        const auto &external_memory = external_properties.externalMemoryProperties;
        bridge_facts_.external_image_supported =
          image_result == VK_SUCCESS &&
          (external_memory.externalMemoryFeatures &
           VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0 &&
          (external_memory.compatibleHandleTypes &
           VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT) != 0;
        interop_dedicated_only_ =
          bridge_facts_.external_image_supported &&
          (external_memory.externalMemoryFeatures &
           VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;

        ComPtr<ID3D11DeviceContext> immediate_context;
        capture_device().GetImmediateContext(immediate_context.GetAddressOf());
        const auto has_interfaces =
          SUCCEEDED(capture_device().QueryInterface(IID_PPV_ARGS(&device5_))) &&
          SUCCEEDED(immediate_context.As(&context4_));
        HRESULT fence_result = E_NOINTERFACE;
        if (has_interfaces) {
          fence_result = device5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                               IID_PPV_ARGS(&interop_fence_));
        }
        bridge_facts_.required_d3d11_interfaces_available =
          has_interfaces && SUCCEEDED(fence_result);
        if (!bridge_facts_.required_d3d11_interfaces_available) {
          interop_fence_.Reset();
        }
        bridge_facts_.shared_fence_or_timeline_semaphore_available =
          interop_fence_ && vulkan_.timeline_semaphore_enabled;

        const VkSemaphoreTypeCreateInfo semaphore_type{
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
          .pNext = nullptr,
          .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
          .initialValue = 0,
        };
        const VkPhysicalDeviceExternalSemaphoreInfo semaphore_info{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
          .pNext = &semaphore_type,
          .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT,
        };
        VkExternalSemaphoreProperties semaphore_properties{
          .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
          .pNext = nullptr,
        };
        vkGetPhysicalDeviceExternalSemaphoreProperties(
          vulkan_.physical_device, &semaphore_info, &semaphore_properties);
        bridge_facts_.timeline_fence_import_available =
          (semaphore_properties.externalSemaphoreFeatures &
           VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0 &&
          (semaphore_properties.compatibleHandleTypes &
           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT) != 0;
        bridge_facts_.win32_import_procedure_available =
          vulkan_.optional_extension_group_enabled &&
          get_memory_handle_properties_ != nullptr &&
          import_semaphore_handle_ != nullptr;
      } catch (...) {
        device5_.Reset();
        context4_.Reset();
        interop_fence_.Reset();
      }
    }

    [[nodiscard]] bool ensure_interop_timeline() noexcept
    {
      if (interop_timeline_ != VK_NULL_HANDLE) {
        return true;
      }
      if (!interop_fence_ || import_semaphore_handle_ == nullptr) {
        return false;
      }
      const VkSemaphoreTypeCreateInfo timeline_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
      };
      const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_info,
        .flags = 0,
      };
      VkSemaphore created_timeline{VK_NULL_HANDLE};
      if (vkCreateSemaphore(vulkan_.device, &semaphore_info, nullptr,
                            &created_timeline) != VK_SUCCESS) {
        return false;
      }
      interop_timeline_ = created_timeline;
      observe_resource(BridgeResourceEvent::SemaphoreCreated);

      HANDLE raw_handle{};
      const auto shared = interop_fence_->CreateSharedHandle(nullptr, GENERIC_ALL,
                                                             nullptr, &raw_handle);
      UniqueWin32Handle handle{raw_handle};
      if (FAILED(shared) || !handle) {
        vkDestroySemaphore(vulkan_.device, interop_timeline_, nullptr);
        observe_resource(BridgeResourceEvent::SemaphoreDestroyed);
        interop_timeline_ = VK_NULL_HANDLE;
        return false;
      }
      const VkImportSemaphoreWin32HandleInfoKHR import_info{
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
        .pNext = nullptr,
        .semaphore = interop_timeline_,
        .flags = 0,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT,
        .handle = handle.get(),
        .name = nullptr,
      };
      if (import_semaphore_handle_(vulkan_.device, &import_info) != VK_SUCCESS) {
        vkDestroySemaphore(vulkan_.device, interop_timeline_, nullptr);
        observe_resource(BridgeResourceEvent::SemaphoreDestroyed);
        interop_timeline_ = VK_NULL_HANDLE;
        return false;
      }
      return true;
    }

    void release_interop_support() noexcept
    {
      if (has_pending_inputs() ||
          std::ranges::any_of(slots_, [](const ProducerSlot &slot) noexcept {
            return slot.interop_image != VK_NULL_HANDLE;
          })) {
        std::terminate();
      }
      if (interop_timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(vulkan_.device, interop_timeline_, nullptr);
        observe_resource(BridgeResourceEvent::SemaphoreDestroyed);
        interop_timeline_ = VK_NULL_HANDLE;
      }
      interop_fence_.Reset();
      context4_.Reset();
      device5_.Reset();
    }

    [[nodiscard]] std::expected<void, core::AppError>
    start_ring(const std::uint64_t serial, const std::uint64_t generation,
               const core::Extent allocation, const DXGI_FORMAT format) noexcept
    {
      if (serial <= last_issued_serial_ || generation == 0 || ring_serial_ != 0 ||
          !valid_allocation(allocation) || format != DXGI_FORMAT_B8G8R8A8_UNORM ||
          d3_device_removed_ || terminal_error_) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }
      ring_serial_ = serial;
      last_issued_serial_ = serial;
      generation_ = generation;
      allocation_ = allocation;
      format_ = format;
      for (std::size_t index{}; index < slots_.size(); ++index) {
        slots_[index].key = {serial, static_cast<std::uint32_t>(index)};
      }
      return {};
    }

    [[nodiscard]] std::expected<InteropRingProbe, core::AppError>
    probe_interop_ring(const std::uint64_t expected_serial) noexcept
    {
      if (expected_serial == 0 || expected_serial != ring_serial_ ||
          get_memory_handle_properties_ == nullptr ||
          std::ranges::any_of(slots_, [](const ProducerSlot &slot) noexcept {
            return slot.staging || slot.query || slot.pending ||
                   slot.interop_image != VK_NULL_HANDLE;
          })) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }
      interop_created_handle_count_ = 0;
      interop_compatible_handle_count_ = 0;
      bool operation_failed{};
      for (auto &slot : slots_) {
        const auto queried = query_interop_slot(slot);
        if (!queried) {
          operation_failed = true;
          break;
        }
        if (!*queried) {
          break;
        }
      }
      bridge_facts_.compatible_memory_type_available =
        interop_created_handle_count_ != 0 &&
        interop_created_handle_count_ == interop_compatible_handle_count_;
      return InteropRingProbe{
        .created_handles = interop_created_handle_count_,
        .compatible_handles = interop_compatible_handle_count_,
        .operation_failed = operation_failed,
      };
    }

    [[nodiscard]] std::expected<render::ExternalSampledImageView, core::AppError>
    finish_interop_slot(const render::SlotKey key) noexcept
    {
      auto *const found = find_slot(key);
      if (found == nullptr || !found->interop_compatible || !found->interop_handle ||
          found->interop_memory != VK_NULL_HANDLE ||
          found->interop_view != VK_NULL_HANDLE || found->pending) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }
      auto &slot = *found;
      const VkMemoryDedicatedAllocateInfo dedicated_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = slot.interop_image,
        .buffer = VK_NULL_HANDLE,
      };
      const VkImportMemoryWin32HandleInfoKHR import_info{
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
        .pNext = slot.interop_requires_dedicated ? &dedicated_info : nullptr,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
        .handle = slot.interop_handle.get(),
        .name = nullptr,
      };
      const VkMemoryAllocateInfo allocation_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = slot.interop_memory_requirements.size,
        .memoryTypeIndex = slot.interop_memory_type_index,
      };
      VkDeviceMemory allocated_interop_memory{VK_NULL_HANDLE};
      const auto result = vkAllocateMemory(vulkan_.device, &allocation_info, nullptr,
                                           &allocated_interop_memory);
      // This import form leaves NT-handle ownership with us. The allocation keeps
      // its own reference on success; either result ends this temporary handle use.
      slot.interop_handle.reset();
      if (result != VK_SUCCESS) {
        return std::unexpected{
          vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
      }
      slot.interop_memory = allocated_interop_memory;
      auto next_result =
        vkBindImageMemory(vulkan_.device, slot.interop_image, slot.interop_memory, 0);
      if (next_result != VK_SUCCESS) {
        return std::unexpected{vulkan_error(
          core::AppErrorCode::RuntimeConstructionFailure, next_result)};
      }
      const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = slot.interop_image,
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
      VkImageView created_interop_view{VK_NULL_HANDLE};
      next_result =
        vkCreateImageView(vulkan_.device, &view_info, nullptr, &created_interop_view);
      if (next_result != VK_SUCCESS) {
        return std::unexpected{vulkan_error(
          core::AppErrorCode::RuntimeConstructionFailure, next_result)};
      }
      slot.interop_view = created_interop_view;
      return render::ExternalSampledImageView{
        .image = slot.interop_image,
        .view = slot.interop_view,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .allocation = allocation_,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
        .external_queue_family = VK_QUEUE_FAMILY_EXTERNAL,
      };
    }

    [[nodiscard]] std::expected<void, core::AppError>
    create_cpu_slot(const render::SlotKey key) noexcept
    {
      auto *const slot = find_slot(key);
      if (slot == nullptr || slot->staging || slot->query || slot->pending ||
          slot->interop_image != VK_NULL_HANDLE) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }
      D3D11_TEXTURE2D_DESC texture_info{};
      texture_info.Width = allocation_.width;
      texture_info.Height = allocation_.height;
      texture_info.MipLevels = 1;
      texture_info.ArraySize = 1;
      texture_info.Format = format_;
      texture_info.SampleDesc.Count = 1;
      texture_info.Usage = D3D11_USAGE_STAGING;
      texture_info.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      const auto result = device_->CreateTexture2D(
        &texture_info, nullptr, slot->staging.GetAddressOf());
      if (FAILED(result)) {
        return std::unexpected{
          d3_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
      }
      return create_query(*slot);
    }

    [[nodiscard]] std::expected<void, core::AppError>
    release_ring(const std::uint64_t expected_serial) noexcept
    {
      if (expected_serial == 0 || expected_serial != ring_serial_ ||
          has_pending_inputs()) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }
      for (auto &slot : slots_) {
        destroy_interop_slot(slot);
      }
      slots_ = {};
      ring_serial_ = 0;
      generation_ = 0;
      allocation_ = {};
      format_ = DXGI_FORMAT_UNKNOWN;
      return {};
    }

    [[nodiscard]] std::expected<BridgeSubmitResult, core::AppError>
    submit(const render::SlotKey key, WindowsCapturedFrame &&frame,
           const render::FrameStamp stamp)
    {
      auto *const slot = find_slot(key);
      const bool external = slot != nullptr && slot->interop_view != VK_NULL_HANDLE;
      const auto content = frame.content_size();
      if (slot == nullptr || slot->pending || !slot->query ||
          d3_device_removed_ || terminal_error_ ||
          frame.resource_generation() != generation_ ||
          frame.allocation_extent() != allocation_ ||
          frame.expected_pool_format() !=
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::
              B8G8R8A8UIntNormalized ||
          content.width == 0 || content.height == 0 ||
          content.width > allocation_.width || content.height > allocation_.height ||
          (!external && !slot->staging) ||
          (external &&
           (slot->staging || !slot->interop_texture || !interop_fence_ ||
            !context4_ || interop_timeline_ == VK_NULL_HANDLE))) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }

      // Projection can throw before adoption. A source texture reference alone
      // never substitutes for keeping the corresponding WGC checkout alive.
      auto source_texture = frame.texture();
      D3D11_TEXTURE2D_DESC source_info{};
      source_texture->GetDesc(&source_info);
      ComPtr<ID3D11Device> source_device;
      source_texture->GetDevice(source_device.GetAddressOf());
      if (source_device.Get() != device_.Get() ||
          source_info.Width < content.width || source_info.Height < content.height ||
          source_info.Format != format_) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }

      const D3D11_BOX source_box{
        .left = 0, .top = 0, .front = 0,
        .right = content.width, .bottom = content.height, .back = 1,
      };
      auto &pending = slot->pending.emplace();
      pending.frame.emplace(std::move(frame));
      pending.content = content;
      pending.stamp = stamp;
      BridgeSubmitResult result;
      if (external) {
        if (next_fence_target_ == (std::numeric_limits<std::uint64_t>::max)()) {
          slot->pending.reset();
          result.count_as_dropped = true;
          result.fallback_requested = true;
          return result;
        }
        pending.external_copy = true;
        pending.target = next_fence_target_++;
        pending.proof = CopyProof::FencePending;
        context_->CopySubresourceRegion(slot->interop_texture.Get(),
                                       0, 0, 0, 0, source_texture.Get(), 0,
                                       &source_box);
        result.d3_copy_bytes =
          static_cast<std::uint64_t>(content.width) * content.height * bytes_per_pixel;
        const auto signal_result =
          context4_->Signal(interop_fence_.Get(), pending.target);
        if (FAILED(signal_result)) {
          // Copy precedes Signal. Healthy-device failure therefore appends query
          // proof and retains the checkout while the caller requests fallback.
          if (FAILED(device_->GetDeviceRemovedReason())) {
            d3_device_removed_ = true;
            slot->pending.reset();
            result.count_as_dropped = true;
            result.d3_device_removed = true;
            return result;
          }
          context_->End(slot->query.Get());
          context_->Flush();
          pending.proof = CopyProof::QueryPending;
          result.fallback_requested = true;
        }
        else {
          context_->Flush();
        }
      }
      else {
        context_->CopySubresourceRegion(slot->staging.Get(), 0, 0, 0, 0,
                                       source_texture.Get(), 0, &source_box);
        result.d3_copy_bytes =
          static_cast<std::uint64_t>(content.width) * content.height * bytes_per_pixel;
        context_->End(slot->query.Get());
        context_->Flush();
      }
      result.disposition = BridgeSubmitDisposition::Pending;
      return result;
    }

    [[nodiscard]] BridgeProgress
    progress(const std::chrono::steady_clock::time_point now,
             const bool discard_completed, const CpuConsumer &consume) noexcept
    {
      BridgeProgress result;
      bool discard_remaining = discard_completed;
      for (auto &slot : slots_) {
        if (!slot.pending) {
          continue;
        }
        if (d3_device_removed_) {
          finish(slot, BridgeCompletionKind::SafeDiscard, false, result);
          continue;
        }
        auto &pending = *slot.pending;
        if (pending.proof == CopyProof::FencePending) {
          const auto completion = render::classify_d3_completed_value(
            interop_fence_->GetCompletedValue(), pending.target);
          if (completion == render::D3CompletionStatus::Pending) {
            continue;
          }
          if (completion == render::D3CompletionStatus::DeviceRemoved) {
            d3_device_removed_ = true;
            finish(slot, BridgeCompletionKind::SafeDiscard, false, result);
            continue;
          }
          finish(slot, discard_remaining ? BridgeCompletionKind::SafeDiscard
                                         : BridgeCompletionKind::ExternalReady,
                 discard_remaining, result);
          continue;
        }
        if (pending.proof == CopyProof::QueryPending) {
          result.fallback_requested =
            result.fallback_requested || pending.external_copy;
          BOOL complete{};
          const auto query_result = context_->GetData(
            slot.query.Get(), &complete, sizeof(complete),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
          if (query_result == S_FALSE ||
              (query_result == S_OK && complete == FALSE)) {
            continue;
          }
          if (FAILED(query_result)) {
            if (FAILED(device_->GetDeviceRemovedReason())) {
              d3_device_removed_ = true;
              finish(slot, BridgeCompletionKind::SafeDiscard, false, result);
            }
            else {
              terminal_error_ =
                d3_error(core::AppErrorCode::RuntimeStateLost, query_result);
            }
            // An unexplained query error is not copy proof. Keep its checkout
            // and return another deadline, including after terminal admission.
            continue;
          }
          pending.frame.reset();
          result.made_progress = true;
          if (pending.external_copy) {
            finish(slot, BridgeCompletionKind::SafeDiscard, true, result);
            continue;
          }
          pending.proof = CopyProof::CpuMapPending;
        }
        if (discard_remaining) {
          finish(slot, BridgeCompletionKind::SafeDiscard, true, result);
          continue;
        }
        consume_cpu(slot, consume, result, discard_remaining);
      }
      result.pending_inputs = has_pending_inputs();
      if (result.pending_inputs) {
        result.next_deadline = now + pending_poll_interval;
      }
      result.d3_device_removed = d3_device_removed_;
      result.terminal_error = terminal_error_;
      return result;
    }

    [[nodiscard]] bool has_pending_inputs() const noexcept
    {
      return std::ranges::any_of(slots_, [](const ProducerSlot &slot) noexcept {
        return slot.pending.has_value();
      });
    }

    void note_d3_device_removed() noexcept { d3_device_removed_ = true; }

  private:
    void destroy_interop_slot(ProducerSlot &slot) noexcept
    {
      slot.interop_handle.reset();
      if (vulkan_.device != VK_NULL_HANDLE) {
        if (slot.interop_view != VK_NULL_HANDLE) {
          vkDestroyImageView(vulkan_.device, slot.interop_view, nullptr);
        }
        if (slot.interop_image != VK_NULL_HANDLE) {
          vkDestroyImage(vulkan_.device, slot.interop_image, nullptr);
        }
        if (slot.interop_memory != VK_NULL_HANDLE) {
          vkFreeMemory(vulkan_.device, slot.interop_memory, nullptr);
        }
      }
      slot.interop_texture.Reset();
      slot.interop_view = VK_NULL_HANDLE;
      slot.interop_image = VK_NULL_HANDLE;
      slot.interop_memory = VK_NULL_HANDLE;
      slot.interop_compatible = false;
    }

    [[nodiscard]] std::expected<bool, core::AppError>
    query_interop_slot(ProducerSlot &slot) noexcept
    {
      const VkExternalMemoryImageCreateInfo external_info{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
      };
      const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent =
          {
            .width = allocation_.width,
            .height = allocation_.height,
            .depth = 1,
          },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      VkImage created_interop_image{VK_NULL_HANDLE};
      auto result =
        vkCreateImage(vulkan_.device, &image_info, nullptr, &created_interop_image);
      if (result != VK_SUCCESS) {
        return std::unexpected{
          vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
      }
      slot.interop_image = created_interop_image;

      VkMemoryDedicatedRequirements dedicated_requirements{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
        .pNext = nullptr,
      };
      VkMemoryRequirements2 requirements{
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated_requirements,
      };
      const VkImageMemoryRequirementsInfo2 requirements_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .pNext = nullptr,
        .image = slot.interop_image,
      };
      vkGetImageMemoryRequirements2(vulkan_.device, &requirements_info, &requirements);

      D3D11_TEXTURE2D_DESC texture_info{};
      texture_info.Width = allocation_.width;
      texture_info.Height = allocation_.height;
      texture_info.MipLevels = 1;
      texture_info.ArraySize = 1;
      texture_info.Format = format_;
      texture_info.SampleDesc.Count = 1;
      texture_info.Usage = D3D11_USAGE_DEFAULT;
      texture_info.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      // NT handles select the handle type, not shared storage. Synchronization
      // stays with the existing shared fence; this is not a keyed-mutex texture.
      texture_info.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
      auto d3_result = capture_device().CreateTexture2D(
        &texture_info, nullptr, slot.interop_texture.GetAddressOf());
      if (FAILED(d3_result)) {
        return std::unexpected{
          d3_error(core::AppErrorCode::RuntimeConstructionFailure, d3_result)};
      }
      if (auto created = create_query(slot); !created) {
        return std::unexpected{created.error()};
      }

      ComPtr<IDXGIResource1> shared_resource;
      d3_result = slot.interop_texture.As(&shared_resource);
      if (FAILED(d3_result)) {
        return std::unexpected{
          d3_error(core::AppErrorCode::RuntimeConstructionFailure, d3_result)};
      }
      HANDLE raw_handle{};
      d3_result = shared_resource->CreateSharedHandle(
        nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
        &raw_handle);
      UniqueWin32Handle handle{raw_handle};
      if (FAILED(d3_result) || !handle) {
        return std::unexpected{
          d3_error(core::AppErrorCode::RuntimeConstructionFailure, d3_result)};
      }
      slot.interop_handle = std::move(handle);
      ++interop_created_handle_count_;

      VkMemoryWin32HandlePropertiesKHR handle_properties{
        .sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR,
        .pNext = nullptr,
      };
      result = get_memory_handle_properties_(
        vulkan_.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
        slot.interop_handle.get(), &handle_properties);
      if (result != VK_SUCCESS) {
        return false;
      }
      // A format-level import capability cannot choose memory for this handle.
      // Both the image requirements and the exported allocation must allow it.
      const auto compatible_bits = requirements.memoryRequirements.memoryTypeBits &
                                   handle_properties.memoryTypeBits;
      const auto memory_index =
        memory_type_index(vulkan_.memory_properties, compatible_bits, 0,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (!memory_index) {
        return false;
      }
      ++interop_compatible_handle_count_;
      slot.interop_compatible = true;
      slot.interop_memory_requirements = requirements.memoryRequirements;
      slot.interop_memory_type_index = *memory_index;
      slot.interop_requires_dedicated =
        interop_dedicated_only_ ||
        dedicated_requirements.requiresDedicatedAllocation == VK_TRUE;
      return true;
    }

    void observe_resource(const BridgeResourceEvent event) noexcept
    {
      if (observer_.observe != nullptr) {
        observer_.observe(observer_.context, event);
      }
    }

    [[nodiscard]] ProducerSlot *find_slot(const render::SlotKey key) noexcept
    {
      if (!render::matches_slot_key(key, ring_serial_, slots_.size())) {
        return nullptr;
      }
      return &slots_[key.index];
    }

    [[nodiscard]] std::expected<void, core::AppError>
    create_query(ProducerSlot &slot) noexcept
    {
      D3D11_QUERY_DESC query_info{};
      query_info.Query = D3D11_QUERY_EVENT;
      const auto result = device_->CreateQuery(&query_info, slot.query.GetAddressOf());
      if (FAILED(result)) {
        return std::unexpected{
          d3_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
      }
      return {};
    }

    // One pass visits each fixed slot once, so completing a record cannot exceed
    // batch capacity. Publish the host-observed wait before releasing its checkout.
    void finish(ProducerSlot &slot, const BridgeCompletionKind kind,
                const bool count_as_dropped, BridgeProgress &result,
                const std::optional<core::AppError> error = std::nullopt) noexcept
    {
      if (!slot.pending || result.completion_count == result.completions.size()) {
        std::terminate();
      }
      const auto &pending = *slot.pending;
      result.completions[result.completion_count++] = {
        .key = slot.key,
        .kind = kind,
        .content = pending.content,
        .stamp = pending.stamp,
        .external_wait = kind == BridgeCompletionKind::ExternalReady
                           ? render::CompletedProducerWait{interop_timeline_, pending.target}
                           : render::CompletedProducerWait{},
        .count_as_dropped = count_as_dropped,
        .error = error,
      };
      slot.pending.reset();
      result.made_progress = true;
    }

    void consume_cpu(ProducerSlot &slot, const CpuConsumer &consume,
                     BridgeProgress &result, bool &discard_remaining) noexcept
    {
      D3D11_MAPPED_SUBRESOURCE mapped{};
      const auto map_result = context_->Map(
        slot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (map_result == DXGI_ERROR_WAS_STILL_DRAWING) {
        return;
      }
      if (FAILED(map_result)) {
        if (FAILED(device_->GetDeviceRemovedReason())) {
          d3_device_removed_ = true;
          finish(slot, BridgeCompletionKind::SafeDiscard, false, result);
        }
        else {
          terminal_error_ = d3_error(core::AppErrorCode::RuntimeStateLost, map_result);
          finish(slot, BridgeCompletionKind::SafeDiscard, false, result, terminal_error_);
        }
        return;
      }
      const MappedReadback unmap{*context_.Get(), *slot.staging.Get()};
      const auto &pending = *slot.pending;
      const auto row_bytes =
        static_cast<std::size_t>(pending.content.width) * bytes_per_pixel;
      const auto last_row = static_cast<std::size_t>(pending.content.height - 1);
      constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
      if (mapped.pData == nullptr || mapped.RowPitch < row_bytes ||
          (last_row != 0 && mapped.RowPitch > (maximum - row_bytes) / last_row)) {
        terminal_error_ = bridge_error(core::AppErrorCode::RuntimeStateLost);
        finish(slot, BridgeCompletionKind::SafeDiscard, false, result, terminal_error_);
        return;
      }
      const render::CpuRowsView rows{
        .bytes = {static_cast<const std::byte *>(mapped.pData),
                  last_row * mapped.RowPitch + row_bytes},
        .row_pitch = mapped.RowPitch,
        .content = pending.content,
        .allocation = allocation_,
      };
      std::expected<void, core::AppError> consumed;
      try {
        consumed = consume(slot.key, rows, pending.stamp);
      } catch (const core::AppError &error) {
        consumed = std::unexpected{error};
      } catch (const std::bad_alloc &) {
        consumed = std::unexpected{
          d3_error(core::AppErrorCode::RuntimeStateLost, E_OUTOFMEMORY)};
      } catch (...) {
        consumed = std::unexpected{bridge_error(core::AppErrorCode::RuntimeStateLost)};
      }
      if (!consumed) {
        // Copy proof was established before Map. A consumer error can safely
        // discard this input, but never discharges another slot's pending copy.
        finish(slot, BridgeCompletionKind::CpuConsumerFailed, false, result,
               consumed.error());
        discard_remaining = true;
        return;
      }
      finish(slot, BridgeCompletionKind::CpuConsumed, false, result);
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Device5> device5_;
    ComPtr<ID3D11DeviceContext4> context4_;
    ComPtr<ID3D11Fence> interop_fence_;
    // Non-owning device dependency. The aggregate explicitly destroys this bridge
    // after renderer detach and before destroying the borrowed VkDevice.
    render::VulkanDeviceBorrow vulkan_;
    render::BridgeCapabilityFacts bridge_facts_{};
    PFN_vkGetMemoryWin32HandlePropertiesKHR get_memory_handle_properties_{};
    PFN_vkImportSemaphoreWin32HandleKHR import_semaphore_handle_{};
    VkSemaphore interop_timeline_{VK_NULL_HANDLE};
    bool interop_dedicated_only_{};
    std::uint64_t next_fence_target_{1};
    std::size_t interop_created_handle_count_{};
    std::size_t interop_compatible_handle_count_{};
    BridgeResourceObserver observer_;
    std::array<ProducerSlot, slot_count> slots_{};
    std::uint64_t ring_serial_{};
    std::uint64_t last_issued_serial_{};
    std::uint64_t generation_{};
    core::Extent allocation_{};
    DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
    bool d3_device_removed_{};
    std::optional<core::AppError> terminal_error_;
  };

  WindowsFrameBridge::WindowsFrameBridge(const BridgeResourceObserver observer)
      : implementation_{std::make_unique<Impl>(observer)} {}
  WindowsFrameBridge::~WindowsFrameBridge() noexcept = default;

  ID3D11Device &WindowsFrameBridge::capture_device() noexcept
  {
    return implementation_->capture_device();
  }

  render::VulkanStartupRequest WindowsFrameBridge::startup_request() const
  {
    return implementation_->startup_request();
  }

  void WindowsFrameBridge::attach_vulkan_device(
    const render::VulkanDeviceBorrow device) noexcept
  {
    implementation_->attach_vulkan_device(device);
  }

  render::BridgeCapabilityFacts WindowsFrameBridge::capability_facts() const noexcept
  {
    return implementation_->capability_facts();
  }

  void WindowsFrameBridge::refresh_interop_capabilities() noexcept
  {
    implementation_->refresh_interop_capabilities();
  }

  std::expected<InteropRingProbe, core::AppError>
  WindowsFrameBridge::probe_interop_ring(const std::uint64_t expected_serial) noexcept
  {
    return implementation_->probe_interop_ring(expected_serial);
  }

  std::expected<render::ExternalSampledImageView, core::AppError>
  WindowsFrameBridge::finish_interop_slot(const render::SlotKey key) noexcept
  {
    return implementation_->finish_interop_slot(key);
  }

  bool WindowsFrameBridge::ensure_interop_timeline() noexcept
  {
    return implementation_->ensure_interop_timeline();
  }

  void WindowsFrameBridge::release_interop_support() noexcept
  {
    implementation_->release_interop_support();
  }

  bool WindowsFrameBridge::d3_device_removed() const noexcept
  {
    return implementation_->d3_device_removed();
  }

  std::expected<void, core::AppError>
  WindowsFrameBridge::start_ring(const std::uint64_t serial,
                                 const std::uint64_t generation,
                                 const core::Extent allocation,
                                 const DXGI_FORMAT format) noexcept
  {
    return implementation_->start_ring(serial, generation, allocation, format);
  }

  std::expected<void, core::AppError>
  WindowsFrameBridge::create_cpu_slot(const render::SlotKey key) noexcept
  {
    return implementation_->create_cpu_slot(key);
  }

  std::expected<void, core::AppError>
  WindowsFrameBridge::release_ring(const std::uint64_t expected_serial) noexcept
  {
    return implementation_->release_ring(expected_serial);
  }

  std::expected<BridgeSubmitResult, core::AppError>
  WindowsFrameBridge::submit(const render::SlotKey key, WindowsCapturedFrame &&frame,
                             const render::FrameStamp stamp)
  {
    return implementation_->submit(key, std::move(frame), stamp);
  }

  BridgeProgress WindowsFrameBridge::progress(
    const std::chrono::steady_clock::time_point now, const bool discard_completed,
    const CpuConsumer &consume) noexcept
  {
    return implementation_->progress(now, discard_completed, consume);
  }

  bool WindowsFrameBridge::has_pending_inputs() const noexcept
  {
    return implementation_->has_pending_inputs();
  }

  void WindowsFrameBridge::note_d3_device_removed() noexcept
  {
    implementation_->note_d3_device_removed();
  }

}    // namespace avb::platform::windows
