/// @file
/// @brief Private definition of the replica renderer's single resource owner.
/// Device bootstrap, frame processing and WSI translation units share this state
/// so canonical slot identity and native lifetime remain attached to one Impl.
#ifndef AVB0795_RENDER_DETAIL_VULKAN_RENDERER_IMPL_HPP
#define AVB0795_RENDER_DETAIL_VULKAN_RENDERER_IMPL_HPP

#include "avb/diagnostics/performance_report.hpp"
#include "render/graphics_retry_policy.hpp"
#include "render/vulkan_renderer.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

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

namespace avb::render::detail {
    inline constexpr std::size_t cpu_slot_count = 3;
    inline constexpr std::size_t present_record_count = cpu_slot_count;

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

}    // namespace avb::render::detail

namespace avb::render {
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

    void observe_performance(const diagnostics::PerformanceCounter counter,
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
    detail::MaintenanceVariant maintenance_variant_{detail::MaintenanceVariant::None};
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
    std::array<CaptureSlot, detail::cpu_slot_count> slots_{};
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
    std::array<std::optional<PresentRecord>, detail::present_record_count>
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

}    // namespace avb::render

#endif
