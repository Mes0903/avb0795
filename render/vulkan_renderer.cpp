/// @file
/// @brief Replica-only native graphics owner. Main-thread ticks advance a bounded
/// Producer-to-Vulkan ring and independent WSI retirement without synchronous waits.
#include "render/vulkan_renderer.hpp"
#include "avb/diagnostics/performance_report.hpp"

#include "avb/render/replica_shaders.hpp"
#include "render/graphics_retry_policy.hpp"
#include "render/render_math.hpp"
#include "render/vulkan_input.hpp"

#include <vulkan/vulkan_core.h>

#include <GLFW/glfw3.h>

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
  namespace {

    using namespace std::chrono_literals;

    constexpr std::size_t cpu_slot_count = 3;
    constexpr std::size_t present_record_count = cpu_slot_count;
    constexpr std::uint32_t bytes_per_pixel = 4;
    constexpr auto pending_poll_interval = 2ms;

    /// @brief Four vec4 blocks shared with the fragment shader's push layout.
    /// Bounds address texel centers; the selection stroke uses framebuffer-pixel
    /// thickness converted to local UV units. The triangle stays allocation-free.
    struct alignas(16) ReplicaPushConstants {
      std::array<float, 4> transform;
      std::array<float, 4> bounds;
      std::array<float, 4> selection;
      std::array<float, 4> stroke;
    };
    static_assert(sizeof(ReplicaPushConstants) == 64);


#if defined(VK_KHR_surface_maintenance1) && \
  defined(VK_KHR_swapchain_maintenance1) && \
  defined(VK_KHR_get_surface_capabilities2)
#define AVB0795_HAS_KHR_MAINTENANCE_VARIANT 1
#endif

#if defined(VK_EXT_surface_maintenance1) && \
  defined(VK_EXT_swapchain_maintenance1) && \
  defined(VK_KHR_get_surface_capabilities2)
#define AVB0795_HAS_EXT_MAINTENANCE_VARIANT 1
#endif

    /// @brief Feature/entry-point pair enabled on this concrete Vulkan device.
    enum class MaintenanceVariant { None,
                                    Khr,
                                    Ext };

    [[nodiscard]] constexpr core::AppError graphics_error(
      const core::AppErrorCode code,
      const std::optional<std::int64_t> detail = std::nullopt) noexcept
    {
      return {.code = code, .native_detail = detail};
    }

    [[nodiscard]] constexpr core::AppError
    vulkan_error(const core::AppErrorCode code, const VkResult result) noexcept
    {
      return graphics_error(code, static_cast<std::int64_t>(result));
    }

    [[nodiscard]] constexpr bool
    has_zero_dimension(const core::Extent value) noexcept
    {
      return value.width == 0 || value.height == 0;
    }

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

    // Independent native obligations share one wakeup; keep the earliest deadline
    // so a later retry cannot postpone an already-pending completion observation.
    template <typename Value>
    void earlier_deadline(std::optional<Value> &current, const Value candidate)
    {
      if (!current || candidate < *current) {
        current = candidate;
      }
    }

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

  class VulkanRenderer::Impl final {
  public:
    using SlotPhase = RenderSlotPhase;

    /// @brief One of three stable ring slots, with CPU or imported-GPU destination.
    /// The key names bridge-owned source work; sampled images survive its source
    /// checkout and can remain as safe redraw content after submission retires.
    struct CaptureSlot final {
      SlotKey key;
      core::Extent content{};
      SlotPhase phase{SlotPhase::Empty};
      FrameStamp stamp;
      // Reservation rollback keeps the old sampled content and wait untouched;
      // only pre-adoption cancellation may restore its retained designation.
      bool restore_retained_on_cancel{};
      PresentationSnapshot recorded_presentation{};

      // CPU transport owns host-visible upload storage plus a separate sampled
      // image. mapped_upload only borrows the mapped upload_memory allocation.
      VkBuffer upload_buffer{VK_NULL_HANDLE};
      VkDeviceMemory upload_memory{VK_NULL_HANDLE};
      void *mapped_upload{};
      VkDeviceSize upload_allocation_size{};
      bool upload_coherent{};
      VkImage sampled_image{VK_NULL_HANDLE};
      VkDeviceMemory sampled_memory{VK_NULL_HANDLE};
      VkImageView sampled_view{VK_NULL_HANDLE};
      bool sampled_image_initialized{};
      // Slot-owned acquisition/submission proof is separate from a present record.
      // The command buffer is pool-owned and survives capture-slot reconstruction.
      VkDescriptorSet descriptor{VK_NULL_HANDLE};
      VkCommandBuffer command_buffer{VK_NULL_HANDLE};
      VkFence acquire_fence{VK_NULL_HANDLE};
      VkFence submission_fence{VK_NULL_HANDLE};
      VkSemaphore acquire_semaphore{VK_NULL_HANDLE};
      VkSemaphore render_finished_semaphore{VK_NULL_HANDLE};
      VkSwapchainKHR owning_swapchain{VK_NULL_HANDLE};
      std::uint32_t acquired_image_index{};
      bool acquired_image{};

      // The bridge owns imported resources; the renderer keeps only registration
      // and host-confirmed producer waits through submission and retained redraw.
      ExternalSampledImageView external_image;
      CompletedProducerWait producer_wait;
      bool gpu_copy{};
    };

    /// @brief Independent presentation lease: semaphore and WSI outlive the submission
    /// record until the attached maintenance1 present fence proves retirement.
    struct PresentRecord final {
      VkFence fence{VK_NULL_HANDLE};
      VkSemaphore wait_semaphore{VK_NULL_HANDLE};
      VkSwapchainKHR swapchain{VK_NULL_HANDLE};
      bool counts_as_presented{};
      bool confirms_recovery{};
      bool uncertain{};
      std::optional<PresentationSnapshot> presentation;
    };

    /// @brief Active or retired WSI aggregate with explicit native teardown.
    ///
    /// Images are borrowed from the swapchain; views/framebuffers/pipeline are
    /// owned children. Retirement preserves them until all referencing slot and
    /// present records are safe, even if replacement construction fails.
    struct SwapchainState final {
      VkSwapchainKHR handle{VK_NULL_HANDLE};
      VkFormat format{VK_FORMAT_UNDEFINED};
      VkExtent2D extent{};
      VkPresentModeKHR present_mode{VK_PRESENT_MODE_FIFO_KHR};
      VkRenderPass render_pass{VK_NULL_HANDLE};
      VkPipeline pipeline{VK_NULL_HANDLE};
      std::vector<VkImage> images;
      std::vector<VkImageView> views;
      std::vector<VkFramebuffer> framebuffers;
      bool recovery_candidate{};
    };

    /// @brief Owner-local construction budget, retained until a replacement presents.
    struct RecoveryEpisode final {
      GraphicsFailureCategory category{GraphicsFailureCategory::Swapchain};
      GraphicsRecoveryEpisodeState state{};
      std::chrono::steady_clock::time_point started_at{};
    };


    Impl(GLFWwindow *window, std::uint64_t generation, core::Extent framebuffer, const VulkanStartupRequest &startup, diagnostics::PerformanceReport *performance, std::uint64_t observation_instance);
    ~Impl() noexcept;
    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    void detach_for_shutdown() noexcept;
    std::expected<std::uint64_t, core::AppError> begin_ring(core::Extent allocation) noexcept;
    std::expected<void, core::AppError> publish_ring(std::uint64_t serial, std::uint64_t generation) noexcept;
    void discard_ready_inputs() noexcept;
    void begin_recovery_drain(bool device_lost) noexcept;
    void contain_failure(VulkanTickResult &result, core::AppError error) noexcept;
    VulkanTickResult begin_tick(core::Extent framebuffer, std::chrono::steady_clock::time_point now) noexcept;
    void retire_submissions(VulkanTickResult &result, std::chrono::steady_clock::time_point now) noexcept;
    void finish_tick(VulkanTickResult &result, std::chrono::steady_clock::time_point now, bool allow_present, bool allow_recovery) noexcept;
    void observe_metadata(VulkanObservationContext context, std::chrono::steady_clock::time_point now) noexcept;

    void observe_performance(const PerformanceCounter counter,
                             const std::uint64_t value = 1) noexcept;
    void release_renderer_resources() noexcept;
    void create_vulkan_instance();
    void create_surface();
    void select_physical_device(
      const VulkanStartupRequest &request);
    void create_vulkan_device(
      const VulkanStartupRequest &request);
    void create_vulkan_common_resources();
    void destroy_vulkan_common_resources() noexcept;
    std::expected<void, core::AppError>
    create_swapchain(const core::Extent requested);
    std::expected<void, core::AppError>
    create_swapchain_render_resources(
      SwapchainState &state);
    void destroy_swapchain_state(
      SwapchainState &state) noexcept;
    void destroy_swapchain() noexcept;
    void destroy_retired_swapchains() noexcept;
    void retire_active_swapchain();
    std::optional<std::uint32_t> find_memory_type(
      const std::uint32_t bits, const VkMemoryPropertyFlags required, const VkMemoryPropertyFlags preferred = 0) const noexcept;
    std::expected<void, core::AppError>
    create_buffer_memory(CaptureSlot &slot,
                         const VkDeviceSize size);
    std::expected<void, core::AppError>
    create_image_memory(CaptureSlot &slot);
    std::expected<void, core::AppError>
    recreate_render_finished(CaptureSlot &slot);
    std::expected<void, core::AppError>
    recreate_acquire_semaphore(CaptureSlot &slot);
    std::expected<void, core::AppError>
    create_slot_presentation_resources(
      CaptureSlot &slot, const VkImageView view, const VkImageLayout layout);
    std::expected<void, core::AppError>
    create_capture_slot(CaptureSlot &slot);
    void destroy_capture_slot(
      CaptureSlot &slot) noexcept;
    void destroy_descriptor_pool() noexcept;
    std::expected<void, core::AppError>
    detach_ring(
      const std::uint64_t expected_ring_serial) noexcept;
    void release_renderer_ring_resources() noexcept;
    bool ready_for_frame() const noexcept;
    CaptureSlot *
    empty_slot() noexcept;
    std::optional<SlotKey> reserve_source_slot() noexcept;
    std::expected<void, core::AppError>
    cancel_source_reservation(const SlotKey key) noexcept;
    std::expected<void, core::AppError>
    register_external_image(
      const SlotKey key, const ExternalSampledImageView image) noexcept;
    std::expected<void, core::AppError>
    complete_external_input(
      const SlotKey key, const core::Extent content, const FrameStamp stamp, const CompletedProducerWait wait) noexcept;
    std::expected<void, core::AppError>
    discard_completed_source(const SlotKey key) noexcept;
    CaptureSlot *
    find_slot(const SlotKey key,
              const SlotPhase phase) noexcept;
    bool has_unsafe_native_work() const noexcept;
    SwapchainOwnershipFacts swapchain_ownership(
      const VkSwapchainKHR handle) const noexcept;
    std::size_t slots_in_flight() const noexcept;
    std::size_t presentations_in_flight() const noexcept;
    bool render_allocation_idle() const noexcept;
    CaptureSlot *
    ready_slot() noexcept;
    PresentRecord *
    empty_present_record() noexcept;
    void destroy_present_records() noexcept;
    void set_presentation(
      const ReplicaPresentation presentation) noexcept;
    bool has_redraw_work() const noexcept;
    void invalidate_presentation() noexcept;
    void resize(const core::Extent framebuffer) noexcept;
    void begin_drain() noexcept;
    void mark_vulkan_device_lost(
      VulkanTickResult &result) noexcept;
    void set_terminal(
      const VkResult result, VulkanTickResult &tick_result) noexcept;
    void schedule_poll(
      VulkanTickResult &result,
      const std::chrono::steady_clock::time_point now) const noexcept;
    void progress_present_records(
      VulkanTickResult &result, const std::chrono::steady_clock::time_point now);
    std::expected<void, core::AppError>
    complete_cpu_input(
      const SlotKey key, const CpuRowsView rows, const FrameStamp stamp) noexcept;
    VkResult record_commands(
      CaptureSlot &slot, const std::uint32_t image_index) noexcept;
    void reset_slot_after_vulkan(CaptureSlot &slot);
    VkResult release_swapchain_image(
      const VkSwapchainKHR swapchain, const std::uint32_t image_index) noexcept;
    void release_acquired_image(
      CaptureSlot &slot, VulkanTickResult &result);
    void progress_submission(
      CaptureSlot &slot, VulkanTickResult &result, const std::chrono::steady_clock::time_point now);
    void progress_slots(
      VulkanTickResult &result, const std::chrono::steady_clock::time_point now);
    void try_present(
      CaptureSlot &slot, PresentRecord &record, VulkanTickResult &result, const std::chrono::steady_clock::time_point now);
    void clear_release_retained_after_swapchain_destroy(
      const VkSwapchainKHR handle);
    void progress_retired_swapchains(
      VulkanTickResult &result);
    void begin_recovery_episode(
      const GraphicsFailureCategory category,
      const std::chrono::steady_clock::time_point now);
    void note_recovery_failure(
      const core::AppError &error, VulkanTickResult &result, const std::chrono::steady_clock::time_point now);
    void reset_recovery_after_present(
      const VkSwapchainKHR handle) noexcept;
    void progress_recreation(
      VulkanTickResult &result, const std::chrono::steady_clock::time_point now);

    // Borrowed window outlives all surfaces; generation is supplied by runtime.
    GLFWwindow *window_{};
    diagnostics::PerformanceReport *performance_{};
    std::uint64_t observation_instance_{};
    diagnostics::PerformanceHighWater performance_live_{};
    std::array<char, 32> performance_driver_{};
    std::optional<std::chrono::steady_clock::time_point> last_present_observed_at_;
    std::uint64_t generation_{};
    core::Extent framebuffer_{};
    core::Extent capture_allocation_{};


    // WSI and graphics ownership belongs only to this replica, not the controller.
    VkInstance instance_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memory_properties_{};
    VkPhysicalDeviceProperties physical_properties_{};
    VkDevice device_{VK_NULL_HANDLE};
    std::uint32_t graphics_queue_family_{};
    std::uint32_t present_queue_family_{};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    VkQueue present_queue_{VK_NULL_HANDLE};
    MaintenanceVariant maintenance_variant_{MaintenanceVariant::None};
    bool optional_extension_group_enabled_{};
    bool timeline_semaphore_supported_{};
    bool khr_surface_maintenance_enabled_{};
    bool ext_surface_maintenance_enabled_{};
#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
    PFN_vkReleaseSwapchainImagesKHR release_images_khr_{};
#endif
#if defined(AVB0795_HAS_EXT_MAINTENANCE_VARIANT)
    PFN_vkReleaseSwapchainImagesEXT release_images_ext_{};
#endif

    // Device-scoped resources survive WSI replacement. Slot rebuild resets the
    // descriptor pool only after slot users retire; device teardown frees pools
    // after their slot and swapchain children have been released.
    VkDescriptorSetLayout descriptor_layout_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    // Stable addresses permit one borrowed retained_slot_ without another owner.
    std::array<CaptureSlot, cpu_slot_count> slots_{};
    std::uint64_t last_ring_serial_{};
    std::uint64_t ring_serial_{};
    // Private identities exist during construction; admission is published only
    // after every native slot and its presentation resources were established.
    bool ring_ready_{};
    // Borrowed from the stable ring; Empty still means there is no native work.
    // Prefer another empty slot for capture, but keep all three reusable.
    CaptureSlot *retained_slot_{};
    std::uint64_t last_submitted_sequence_{};
    bool redraw_requested_{};
    ReplicaPresentation presentation_;
    bool presentation_dirty_{};
    std::optional<PresentationSnapshot> published_presentation_;
    std::uint64_t next_present_sequence_{1};
    std::uint64_t first_valid_present_sequence_{1};
    // Present records are bounded independently of reusable source-copy slots.
    std::array<std::optional<PresentRecord>, present_record_count>
      present_records_{};
    std::optional<SwapchainState> swapchain_;
    std::vector<SwapchainState> retired_swapchains_;


    // Admission, recovery, and completion are separate state. Closing admission
    // or recording a terminal error leaves native owners available to later ticks.
    bool accepting_frames_{true};
    bool draining_{};
    bool progress_pending_{};
    bool swapchain_recreate_pending_{};
    bool surface_recovery_pending_{};
    bool surface_recovery_active_{};
    bool vulkan_device_lost_{};
    bool fully_constructed_{};
    bool shutdown_detached_{};
    std::optional<core::AppError> terminal_error_;
    std::optional<RecoveryEpisode> recovery_episode_;
    std::optional<core::AppError> recovery_last_error_;
    std::uint64_t presented_frames_{};
    std::uint64_t dropped_frames_{};
    std::uint64_t gpu_interop_frames_submitted_{};
  };

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

  std::optional<std::uint32_t> VulkanRenderer::Impl::find_memory_type(
    const std::uint32_t bits, const VkMemoryPropertyFlags required, const VkMemoryPropertyFlags preferred) const noexcept
  {
    return memory_type_index(memory_properties_, bits, required, preferred);
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

  void VulkanRenderer::Impl::destroy_descriptor_pool() noexcept
  {
    if (device_ != VK_NULL_HANDLE && descriptor_pool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
      descriptor_pool_ = VK_NULL_HANDLE;
    }
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
