/// @file
/// @brief Replica-only native graphics owner. Main-thread ticks advance a bounded
/// D3-to-Vulkan ring and independent WSI retirement without synchronous waits.
#include "render/windows_render_stack.hpp"
#include "avb/diagnostics/performance_report.hpp"

#include "avb/render/replica_shaders.hpp"
#include "platform/windows/detail/windows_graphics_policy.hpp"
#include "platform/windows/overlay_window.hpp"
#include "platform/windows/windows_captured_frame.hpp"
#include "render/graphics_retry_policy.hpp"
#include "render/render_math.hpp"
#include "render/vulkan_input.hpp"

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <GLFW/glfw3.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

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

    using Microsoft::WRL::ComPtr;
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

    [[nodiscard]] constexpr core::AppError d3_error(const core::AppErrorCode code,
                                                    const HRESULT result) noexcept
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

  /// @brief Canonical owner for one replica graphics generation. The runtime owns this
  /// aggregate and its borrowed overlay; callbacks never enter this data plane.
  class WindowsRenderStack::Impl final {
  public:
    using SlotPhase = RenderSlotPhase;

    /// @brief One of three stable ring slots, with CPU or imported-GPU destination.
    /// frame owns only the WGC source-copy lease; sampled images survive that
    /// checkout and can remain as safe redraw content after submission retires.
    struct CaptureSlot final {
      // Source-copy lease and completion query. These can outlive Vulkan loss
      // because the D3 device has an independent completion/removal obligation.
      ComPtr<ID3D11Texture2D> readback_texture;
      ComPtr<ID3D11Query> copy_query;
      std::optional<platform::windows::WindowsCapturedFrame> frame;
      core::Extent content{};
      SlotPhase phase{SlotPhase::Empty};
      FrameStamp stamp;
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

      // GPU transport shares a D3 destination allocation with Vulkan. Its source
      // checkout is still released after the D3 copy, before Vulkan samples it.
      ComPtr<ID3D11Texture2D> interop_texture;
      UniqueWin32Handle interop_handle;
      VkImage interop_image{VK_NULL_HANDLE};
      VkDeviceMemory interop_memory{VK_NULL_HANDLE};
      VkImageView interop_view{VK_NULL_HANDLE};
      VkMemoryRequirements interop_memory_requirements{};
      std::uint32_t interop_memory_type_index{};
      bool interop_requires_dedicated{};
      std::uint64_t d3_fence_target{};
      bool gpu_copy{};
      bool query_completion{};
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

    /// Construct device and WSI owners around a borrowed overlay on the main
    /// thread; partial setup enqueues no work and can unwind immediately.
    Impl(GLFWwindow *native_window, std::uint64_t resource_generation, core::Extent initial_framebuffer, WindowsBridgePreference bridge_preference,
         diagnostics::PerformanceReport *performance, std::uint64_t observation_instance);
    /// Require established native-use or matching device-loss proof; never wait.
    ~Impl() noexcept;

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    /// Borrow the D3 device while this graphics generation remains owned.
    [[nodiscard]] ID3D11Device &capture_device() const noexcept
    {
      return *d3_device_.Get();
    }

    /// Check admission and bounded ring capacity without pulling a WGC frame.
    [[nodiscard]] bool ready_for_frame() const noexcept;
    /// Adopt a matching checkout and enqueue its D3 copy on the main thread.
    /// Retain the checkout until query/fence completion or confirmed D3 removal.
    void submit(platform::windows::WindowsCapturedFrame &&frame);
    void set_presentation(ReplicaPresentation presentation) noexcept;
    void set_performance_fps(core::FpsCap fps) noexcept { performance_fps_ = fps; }
    [[nodiscard]] bool has_redraw_work() const noexcept;
    void request_redraw() noexcept { redraw_requested_ = true; }
    void invalidate_presentation() noexcept;
    /// Poll native proof and WSI recovery without waiting; contain failures into
    /// fixed result values while preserving possibly referenced owners.
    [[nodiscard]] RenderTickResult
    tick(core::Extent framebuffer,
         std::chrono::steady_clock::time_point now) noexcept;
    /// Prove old slot allocations have no D3/submission users. Ready content
    /// has no native work and can be discarded during the rebuild barrier.
    [[nodiscard]] bool render_allocation_idle() const noexcept;
    /// Rebuild three slots after the runtime's old-allocation barrier; probe real
    /// NT-handle memory compatibility, otherwise construct the CPU ring.
    [[nodiscard]] std::expected<void, core::AppError>
    rebuild_capture_slots(std::uint64_t generation, core::Extent allocation, DXGI_FORMAT format);
    /// Record physical extent; a nonzero change requests swapchain recreation.
    void resize(core::Extent framebuffer) noexcept;
    /// Close new-copy admission and discard ready content; later ticks retire users.
    void begin_drain() noexcept;
    /// Apply a confirmed loss to only its matching API's cleanup obligations.
    void begin_device_recovery_drain(DeviceLossKind kind) noexcept;
    /// Return the runtime-supplied generation, never an independently chosen epoch.
    [[nodiscard]] std::uint64_t generation() const noexcept
    {
      return generation_;
    }
    /// Return concrete capability facts without querying the driver again.
    [[nodiscard]] BridgeCapabilityFacts bridge_capability_facts() const noexcept
    {
      return bridge_facts_;
    }
    /// Return the canonical bridge choice after slot construction selects it.
    [[nodiscard]] std::optional<BridgeSelection>
    bridge_selection() const noexcept
    {
      return bridge_selection_;
    }
    /// Report the transport implemented by the currently owned slot ring.
    [[nodiscard]] core::BridgeMode active_bridge_mode() const noexcept
    {
      return active_bridge_mode_;
    }
    /// Count enqueued interop submissions for concrete-path diagnostics.
    [[nodiscard]] std::uint64_t gpu_interop_frames_submitted() const noexcept
    {
      return gpu_interop_frames_submitted_;
    }
    /// Consume one pending bridge selection for edge-driven runtime events.
    [[nodiscard]] std::optional<BridgeSelection>
    take_bridge_mode_change() noexcept
    {
      return std::exchange(bridge_change_pending_, std::nullopt);
    }

  private:
    void observe_performance(PerformanceCounter counter, std::uint64_t value = 1) noexcept;
    void observe_performance_metadata(std::chrono::steady_clock::time_point now) noexcept;
    /// Create the hardware BGRA device used by WGC and all source copies.
    void create_d3_device();
    /// Enable GLFW surface and mandatory maintenance1 requirements before use.
    void create_vulkan_instance();
    /// Create WSI against the borrowed overlay, whose window must outlive the stack.
    void create_surface();
    /// Prefer the D3 adapter LUID, then require presentation/maintenance1 support.
    /// A LUID mismatch may use CPU copy without bypassing the WSI safety floor.
    void select_physical_device();
    /// Create queues and resolve enabled maintenance/import procedures; optional
    /// interop support must not weaken required presentation safety.
    void create_vulkan_device();
    /// Query real LUID, external-image, D3 interface and timeline-import facts;
    /// per-slot NT-handle compatibility is established separately.
    void collect_bridge_capabilities() noexcept;
    /// Permanently import the shared D3 fence as a Vulkan timeline semaphore;
    /// close the temporary NT handle after import while keeping both fence owners.
    [[nodiscard]] bool create_interop_timeline() noexcept;
    /// Create device-scoped pools, layouts and sampler before any submission.
    void create_vulkan_common_resources();
    /// Build a compatible SDR replacement; retire active before using oldSwapchain.
    /// A failed create leaves that old owner retired instead of reactivating it.
    [[nodiscard]] std::expected<void, core::AppError>
    create_swapchain(core::Extent requested);
    /// Build candidate views, render pass, shader pipeline and framebuffers;
    /// the caller releases the candidate if any allocation fails.
    [[nodiscard]] std::expected<void, core::AppError>
    create_swapchain_render_resources(SwapchainState &state);
    /// Construct CPU staging/query, mapped upload and sampled-image resources.
    [[nodiscard]] std::expected<void, core::AppError>
    create_capture_slot(CaptureSlot &slot);
    /// Create a shared D3 texture and query its NT handle's Vulkan memory-type
    /// intersection; device-level capability alone does not prove a handle usable.
    [[nodiscard]] std::expected<bool, core::AppError>
    query_interop_slot(CaptureSlot &slot);
    /// Import a proven-compatible handle and create its sampling resources;
    /// this slot becomes the canonical owner of the imported Vulkan allocation.
    [[nodiscard]] std::expected<void, core::AppError>
    finish_interop_slot(CaptureSlot &slot);
    /// Create descriptor, command buffer and unsignaled synchronization for a
    /// slot's sampled view; this setup enqueues no GPU work.
    [[nodiscard]] std::expected<void, core::AppError>
    create_slot_presentation_resources(CaptureSlot &slot, VkImageView view, VkImageLayout layout);
    /// Release Vulkan imports before the D3 texture, after native-use/loss proof.
    void destroy_interop_slot(CaptureSlot &slot) noexcept;
    /// Close copy admission and latch fallback without destroying active users.
    void request_interop_fallback() noexcept;
    /// Observe complete interop copy/submission/present retirement, then replace
    /// the ring with CPU resources and emit one bridge result.
    void progress_interop_fallback(RenderTickResult &result);

    /// Release a proven-safe slot while preserving its reusable command buffer.
    void destroy_capture_slot(CaptureSlot &slot) noexcept;
    /// Invalidate the borrowed redraw slot and release all safe slot allocations.
    void destroy_capture_slots() noexcept;
    /// Release present fences/semaphores only with retirement or Vulkan-loss
    /// proof; submission completion alone does not establish presentation safety.
    void destroy_present_records() noexcept;
    /// Destroy one WSI record after its image users and presents are safe.
    void destroy_swapchain_state(SwapchainState &state) noexcept;
    /// Release active WSI during safe teardown or never-submitted partial startup.
    void destroy_swapchain() noexcept;
    /// Release all retired WSI records after teardown safety has been established.
    void destroy_retired_swapchains() noexcept;
    /// Release device-wide pools/layouts after their dependent children.
    void destroy_vulkan_common_resources() noexcept;

    /// Check outstanding D3 and healthy Vulkan obligations before destruction.
    /// Confirmed Vulkan loss never masks a still-pending healthy D3 copy.
    [[nodiscard]] bool has_unsafe_native_work() const noexcept;
    /// Derive image-use, present-fence and release-retention facts for one handle.
    [[nodiscard]] SwapchainOwnershipFacts
    swapchain_ownership(VkSwapchainKHR handle) const noexcept;
    /// Count nonempty proof phases, excluding a safe retained redraw image.
    [[nodiscard]] std::size_t slots_in_flight() const noexcept;
    /// Count independent present records, not ordinary queue submissions.
    [[nodiscard]] std::size_t presentations_in_flight() const noexcept;
    /// Prefer non-retained empty storage while keeping all three slots reusable.
    [[nodiscard]] CaptureSlot *empty_slot() noexcept;
    /// Choose the newest eligible sequence or one requested retained redraw;
    /// older ready content can be dropped because its D3 checkout has retired.
    [[nodiscard]] CaptureSlot *ready_slot() noexcept;
    /// Reserve one of three bounded present records before starting an acquire.
    [[nodiscard]] PresentRecord *empty_present_record() noexcept;

    /// Latch Vulkan loss and stop new work without claiming D3 removal.
    void mark_vulkan_device_lost(RenderTickResult &result) noexcept;
    /// Latch D3 removal without claiming healthy Vulkan work is retired.
    void mark_d3_device_removed(RenderTickResult &result) noexcept;
    /// Stop admission and retain uncertainty for the runtime's safe stop path.
    void set_terminal(VkResult result, RenderTickResult &tick_result) noexcept;
    /// Provide a strictly future observation deadline for pending native work.
    void schedule_poll(RenderTickResult &result,
                       std::chrono::steady_clock::time_point now) const noexcept;
    /// Poll present fences separately from submission fences; release their
    /// wait semaphores and count only successful retired presentations.
    void progress_present_records(RenderTickResult &result,
                                  std::chrono::steady_clock::time_point now);
    /// Destroy records only after image-use and present proof; clear retained
    /// failed-release slots after their swapchain has been legally destroyed.
    void progress_retired_swapchains(RenderTickResult &result);
    /// Advance each slot through the nonblocking proof required by its phase.
    void progress_slots(RenderTickResult &result,
                        std::chrono::steady_clock::time_point now);
    /// Poll D3 completion before checking in WGC. CPU then maps rows into upload
    /// storage; GPU becomes ready only with an already-satisfiable timeline wait.
    void progress_d3_copy(CaptureSlot &slot, RenderTickResult &result, std::chrono::steady_clock::time_point now);
    /// Poll acquire or submission proof by phase, then recycle or release the
    /// acquired image that was never handed to presentation.
    void progress_submission(CaptureSlot &slot, RenderTickResult &result, std::chrono::steady_clock::time_point now);
    /// Invoke the enabled maintenance1 variant for one proven-unused image.
    [[nodiscard]] VkResult
    release_swapchain_image(VkSwapchainKHR swapchain,
                            std::uint32_t image_index) noexcept;
    /// Classify release without clearing image ownership on failure; completed
    /// unconsumed binary signals have their own safe-destruction proof.
    void release_acquired_image(CaptureSlot &slot, RenderTickResult &result);
    /// Recycle synchronization after proven completion and retain the newest
    /// completed sampled image when a future redraw may reuse it.
    void reset_slot_after_vulkan(CaptureSlot &slot);
    /// Record upload or external ownership transfer and the aspect-fit draw.
    /// Retained CPU redraw skips upload; GPU redraw reacquires EXTERNAL ownership.
    [[nodiscard]] VkResult record_commands(CaptureSlot &slot,
                                           std::uint32_t image_index) noexcept;
    /// Acquire at timeout zero, classify submit/present independently, and move
    /// the render-finished semaphore into an enqueued present's record.
    void try_present(CaptureSlot &slot, PresentRecord &record, RenderTickResult &result, std::chrono::steady_clock::time_point now);
    /// Advance bounded active/retired WSI recovery. Surface recovery preserves
    /// capture, D3, slot resources and the runtime-owned generation.
    void progress_recreation(RenderTickResult &result,
                             std::chrono::steady_clock::time_point now);
    /// Recycle failed-release slots only after their owning swapchain was legally
    /// destroyed; a release error itself never proves image ownership ended.
    void clear_release_retained_after_swapchain_destroy(VkSwapchainKHR handle);
    /// Move active WSI into the retired set before attempting replacement.
    void retire_active_swapchain();
    /// Start/promote a WSI episode and invalidate obsolete success markers.
    void begin_recovery_episode(GraphicsFailureCategory category,
                                std::chrono::steady_clock::time_point now);
    /// Account for failed construction and schedule bounded retry; confirmed
    /// device loss instead enters runtime-owned device recovery.
    void note_recovery_failure(const core::AppError &error,
                               RenderTickResult &result,
                               std::chrono::steady_clock::time_point now);
    /// Reset retries only after a replacement's successful present retires.
    void reset_recovery_after_present(VkSwapchainKHR handle) noexcept;

    /// Choose compatible required flags and prefer optional flags when available.
    [[nodiscard]] std::optional<std::uint32_t>
    find_memory_type(std::uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) const noexcept;
    /// Create mapped host-visible upload storage and retain its allocation/coherency
    /// facts for later flushes and teardown.
    [[nodiscard]] std::expected<void, core::AppError>
    create_buffer_memory(CaptureSlot &slot, VkDeviceSize size);
    /// Create the CPU path's device-local UNORM sampled image and view.
    [[nodiscard]] std::expected<void, core::AppError>
    create_image_memory(CaptureSlot &slot);
    /// Allocate a fresh unsignaled semaphore after the old one moved to a present.
    [[nodiscard]] std::expected<void, core::AppError>
    recreate_render_finished(CaptureSlot &slot);
    /// Replace a completed unconsumed acquire signal instead of reusing it signaled.
    [[nodiscard]] std::expected<void, core::AppError>
    recreate_acquire_semaphore(CaptureSlot &slot);

    // Borrowed window outlives all surfaces; generation is supplied by runtime.
    GLFWwindow *window_{};
    diagnostics::PerformanceReport *performance_{};
    std::uint64_t observation_instance_{};
    diagnostics::PerformanceHighWater performance_live_{};
    core::Extent performance_content_{};
    core::FpsCap performance_fps_{core::FpsCap::Fps60};
    std::array<char, 32> performance_driver_{};
    std::optional<std::chrono::steady_clock::time_point> last_present_observed_at_;
    std::uint64_t generation_{};
    WindowsBridgePreference bridge_preference_{};
    core::Extent framebuffer_{};
    core::Extent capture_allocation_{};
    DXGI_FORMAT capture_format_{DXGI_FORMAT_UNKNOWN};

    // D3 source-copy owners are independent of Vulkan device-loss evidence.
    ComPtr<ID3D11Device> d3_device_;
    ComPtr<ID3D11DeviceContext> d3_context_;
    ComPtr<ID3D11Device5> d3_device5_;
    ComPtr<ID3D11DeviceContext4> d3_context4_;
    ComPtr<ID3D11Fence> interop_fence_;

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
    bool interop_device_extensions_supported_{};
    bool timeline_semaphore_supported_{};
    bool khr_surface_maintenance_enabled_{};
    bool ext_surface_maintenance_enabled_{};
#if defined(AVB0795_HAS_KHR_MAINTENANCE_VARIANT)
    PFN_vkReleaseSwapchainImagesKHR release_images_khr_{};
#endif
    PFN_vkGetMemoryWin32HandlePropertiesKHR get_memory_handle_properties_{};
    PFN_vkImportSemaphoreWin32HandleKHR import_semaphore_handle_{};
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
    VkSemaphore interop_timeline_{VK_NULL_HANDLE};
    // Stable addresses permit one borrowed retained_slot_ without another owner.
    std::array<CaptureSlot, cpu_slot_count> slots_{};
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

    // Capability facts are diagnostic values; active_bridge_mode_ describes the
    // actually constructed ring. A pending selection edge owns no native state.
    BridgeCapabilityFacts bridge_facts_{};
    std::optional<BridgeSelection> bridge_selection_;
    std::optional<BridgeSelection> bridge_change_pending_;
    core::BridgeMode active_bridge_mode_{core::BridgeMode::CpuCopy};
    bool bridge_capabilities_collected_{};
    bool interop_timeline_ready_{};
    bool interop_dedicated_only_{};
    bool interop_fallback_pending_{};
    std::uint64_t next_d3_fence_target_{1};
    std::size_t interop_created_handle_count_{};
    std::size_t interop_compatible_handle_count_{};

    // Admission, recovery, and completion are separate state. Closing admission
    // or recording a terminal error leaves native owners available to later ticks.
    bool accepting_frames_{true};
    bool draining_{};
    bool progress_pending_{};
    bool swapchain_recreate_pending_{};
    bool surface_recovery_pending_{};
    bool surface_recovery_active_{};
    bool vulkan_device_lost_{};
    bool d3_device_removed_{};
    bool fully_constructed_{};
    std::optional<core::AppError> terminal_error_;
    std::optional<RecoveryEpisode> recovery_episode_;
    std::optional<core::AppError> recovery_last_error_;
    std::uint64_t captured_frames_{};
    std::uint64_t presented_frames_{};
    std::uint64_t dropped_frames_{};
    std::uint64_t gpu_interop_frames_submitted_{};
  };

  void WindowsRenderStack::Impl::observe_performance(const PerformanceCounter counter,
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

  void WindowsRenderStack::Impl::observe_performance_metadata(
    const std::chrono::steady_clock::time_point now) noexcept
  {
    constexpr std::array reasons{
      "none", "adapter_luid_unavailable_or_mismatch", "external_image_unsupported",
      "required_d3d11_interfaces_unavailable", "shared_fence_or_timeline_unavailable",
      "timeline_import_unavailable", "win32_import_procedure_unavailable",
      "compatible_memory_type_unavailable", "interop_operation_failure"};
    std::uint32_t fps{};
    switch (performance_fps_) {
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
      .os = "Windows",
      .bridge = active_bridge_mode_ == core::BridgeMode::GpuInterop ? "gpu_interop" : "cpu_copy",
      .bridge_reason = bridge_selection_ ? reasons[static_cast<std::size_t>(bridge_selection_->reason)] : "pending",
      .present_mode = mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO",
      .ui_policy = "see controller owner metadata",
      .requested_bridge = bridge_preference_ == WindowsBridgePreference::ForceCpuBaseline ? "cpu" : "auto",
      .content_width = performance_content_.width,
      .content_height = performance_content_.height,
      .allocation_width = capture_allocation_.width,
      .allocation_height = capture_allocation_.height,
      .framebuffer_width = framebuffer_.width,
      .framebuffer_height = framebuffer_.height,
      .fps_cap = fps,
    }, now);
    performance_live_.slots = slots_in_flight();
    performance_live_.presents = presentations_in_flight();
    performance_->observe_high_water(PerformanceOwner::Replica, performance_live_, now);
  }

  WindowsRenderStack::Impl::Impl(GLFWwindow *const native_window,
                                 const std::uint64_t resource_generation,
                                 const core::Extent initial_framebuffer,
                                 const WindowsBridgePreference bridge_preference,
                                 diagnostics::PerformanceReport *performance, const std::uint64_t observation_instance)
      : window_{native_window}, performance_{performance}, observation_instance_{observation_instance}, generation_{resource_generation}, bridge_preference_{bridge_preference}, framebuffer_{initial_framebuffer}
  {
    try {
      if (window_ == nullptr || generation_ == 0) {
        throw graphics_error(core::AppErrorCode::InvalidValue);
      }
      create_d3_device();
      create_vulkan_instance();
      create_surface();
      select_physical_device();
      if (performance_) {
        std::snprintf(performance_driver_.data(), performance_driver_.size(), "raw:%u", physical_properties_.driverVersion);
      }
      create_vulkan_device();
      collect_bridge_capabilities();
      create_vulkan_common_resources();
      if (!has_zero_dimension(framebuffer_)) {
        const auto created = create_swapchain(framebuffer_);
        if (!created) {
          throw created.error();
        }
      }
      fully_constructed_ = true;
    } catch (...) {
      destroy_capture_slots();
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
      throw;
    }
  }

  WindowsRenderStack::Impl::~Impl() noexcept
  {
    // A destructor cannot manufacture completion. The runtime must keep ticking
    // this aggregate, or choose non-unwinding process abandonment if it stalls.
    if (fully_constructed_ && has_unsafe_native_work()) {
      std::terminate();
    }
    destroy_capture_slots();
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

  void WindowsRenderStack::Impl::create_d3_device()
  {
    constexpr std::array requested_levels{D3D_FEATURE_LEVEL_11_1,
                                          D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected_level{};
    const auto result = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested_levels.data(),
      static_cast<UINT>(requested_levels.size()), D3D11_SDK_VERSION,
      d3_device_.GetAddressOf(), &selected_level, d3_context_.GetAddressOf());
    if (FAILED(result)) {
      throw d3_error(core::AppErrorCode::UnsupportedGraphics, result);
    }
    static_cast<void>(selected_level);
  }

  void WindowsRenderStack::Impl::create_vulkan_instance()
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

  void WindowsRenderStack::Impl::create_surface()
  {
    const auto result =
      glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure, result);
    }
  }

  void WindowsRenderStack::Impl::select_physical_device()
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

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> d3_adapter;
    DXGI_ADAPTER_DESC d3_adapter_description{};
    const bool d3_luid_available =
      SUCCEEDED(d3_device_.As(&dxgi_device)) &&
      SUCCEEDED(dxgi_device->GetAdapter(d3_adapter.GetAddressOf())) &&
      SUCCEEDED(d3_adapter->GetDesc(&d3_adapter_description));
    const auto matches_d3_adapter =
      [d3_luid_available, d3_luid = d3_adapter_description.AdapterLuid](
        const VkPhysicalDevice device) noexcept {
        if (!d3_luid_available) {
          return false;
        }
        VkPhysicalDeviceIDProperties id{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
          .pNext = nullptr,
        };
        VkPhysicalDeviceProperties2 properties{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
          .pNext = &id,
        };
        vkGetPhysicalDeviceProperties2(device, &properties);
        return id.deviceLUIDValid == VK_TRUE &&
               std::memcmp(&d3_luid, id.deviceLUID, VK_LUID_SIZE) == 0;
      };
    // Prefer a same-adapter candidate for sharing while preserving the original
    // order of equally ranked devices; later checks still enforce the WSI floor.
    std::ranges::stable_sort(
      devices, [&matches_d3_adapter](const VkPhysicalDevice left,
                                     const VkPhysicalDevice right) noexcept {
        return matches_d3_adapter(left) && !matches_d3_adapter(right);
      });

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
      interop_device_extensions_supported_ =
        extension_available(extensions,
                            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) &&
        extension_available(extensions,
                            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
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
        timeline_features.timelineSemaphore == VK_TRUE;
      return;
    }
    throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
  }

  void WindowsRenderStack::Impl::create_vulkan_device()
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
    if (interop_device_extensions_supported_) {
      extensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
      extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
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
    if (interop_device_extensions_supported_) {
      get_memory_handle_properties_ =
        reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
          vkGetDeviceProcAddr(device_,
                              "vkGetMemoryWin32HandlePropertiesKHR"));
      import_semaphore_handle_ =
        reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
          vkGetDeviceProcAddr(device_, "vkImportSemaphoreWin32HandleKHR"));
    }
  }

  void WindowsRenderStack::Impl::collect_bridge_capabilities() noexcept
  {
    // These queries establish a device-level prefix. Memory compatibility stays
    // unproven until slot creation queries the handles for the actual allocation.
    bridge_capabilities_collected_ = false;
    bridge_facts_ = {};
    d3_device5_.Reset();
    d3_context4_.Reset();
    interop_fence_.Reset();
    try {
      ComPtr<IDXGIDevice> dxgi_device;
      ComPtr<IDXGIAdapter> adapter;
      DXGI_ADAPTER_DESC adapter_description{};
      const auto has_d3_luid =
        SUCCEEDED(d3_device_.As(&dxgi_device)) &&
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
      vkGetPhysicalDeviceProperties2(physical_device_, &properties);
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
        physical_device_, &image_info, &image_properties);
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

      const auto has_interfaces = SUCCEEDED(d3_device_.As(&d3_device5_)) &&
                                  SUCCEEDED(d3_context_.As(&d3_context4_));
      HRESULT fence_result = E_NOINTERFACE;
      if (has_interfaces) {
        fence_result = d3_device5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                                IID_PPV_ARGS(&interop_fence_));
      }
      bridge_facts_.required_d3d11_interfaces_available =
        has_interfaces && SUCCEEDED(fence_result);
      if (!bridge_facts_.required_d3d11_interfaces_available) {
        interop_fence_.Reset();
      }
      bridge_facts_.shared_fence_or_timeline_semaphore_available =
        interop_fence_ && timeline_semaphore_supported_;

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
        physical_device_, &semaphore_info, &semaphore_properties);
      bridge_facts_.timeline_fence_import_available =
        (semaphore_properties.externalSemaphoreFeatures &
         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0 &&
        (semaphore_properties.compatibleHandleTypes &
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT) != 0;
      bridge_facts_.win32_import_procedure_available =
        interop_device_extensions_supported_ &&
        get_memory_handle_properties_ != nullptr &&
        import_semaphore_handle_ != nullptr;
      bridge_capabilities_collected_ = true;
    } catch (...) {
      d3_device5_.Reset();
      d3_context4_.Reset();
      interop_fence_.Reset();
      bridge_capabilities_collected_ = true;
    }
  }

  bool WindowsRenderStack::Impl::create_interop_timeline() noexcept
  {
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
    if (vkCreateSemaphore(device_, &semaphore_info, nullptr,
                          &created_timeline) != VK_SUCCESS) {
      return false;
    }
    interop_timeline_ = created_timeline;
    observe_performance(PerformanceCounter::SemaphoresCreated);

    HANDLE raw_handle{};
    const auto shared = interop_fence_->CreateSharedHandle(nullptr, GENERIC_ALL,
                                                           nullptr, &raw_handle);
    UniqueWin32Handle handle{raw_handle};
    if (FAILED(shared) || !handle) {
      vkDestroySemaphore(device_, interop_timeline_, nullptr);
      observe_performance(PerformanceCounter::SemaphoresDestroyed);
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
    if (import_semaphore_handle_(device_, &import_info) != VK_SUCCESS) {
      vkDestroySemaphore(device_, interop_timeline_, nullptr);
      observe_performance(PerformanceCounter::SemaphoresDestroyed);
      interop_timeline_ = VK_NULL_HANDLE;
      return false;
    }
    interop_timeline_ready_ = true;
    return true;
  }

  void WindowsRenderStack::Impl::create_vulkan_common_resources()
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

  void WindowsRenderStack::Impl::destroy_vulkan_common_resources() noexcept
  {
    if (device_ == VK_NULL_HANDLE) {
      return;
    }
    if (sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, sampler_, nullptr);
      sampler_ = VK_NULL_HANDLE;
    }
    if (interop_timeline_ != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, interop_timeline_, nullptr);
      observe_performance(PerformanceCounter::SemaphoresDestroyed);
      interop_timeline_ = VK_NULL_HANDLE;
      interop_timeline_ready_ = false;
    }
    if (command_pool_ != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device_, command_pool_, nullptr);
      observe_performance(PerformanceCounter::CommandBuffersDestroyed, performance_live_.command_buffers);
      command_pool_ = VK_NULL_HANDLE;
      for (auto &slot : slots_) {
        slot.command_buffer = VK_NULL_HANDLE;
      }
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
      descriptor_pool_ = VK_NULL_HANDLE;
    }
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
  WindowsRenderStack::Impl::create_swapchain(const core::Extent requested)
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
  WindowsRenderStack::Impl::create_swapchain_render_resources(
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

  void WindowsRenderStack::Impl::destroy_swapchain_state(
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

  void WindowsRenderStack::Impl::destroy_swapchain() noexcept
  {
    if (!swapchain_) {
      return;
    }
    destroy_swapchain_state(*swapchain_);
    swapchain_.reset();
  }

  void WindowsRenderStack::Impl::destroy_retired_swapchains() noexcept
  {
    for (auto &state : retired_swapchains_) {
      destroy_swapchain_state(state);
    }
    retired_swapchains_.clear();
  }

  void WindowsRenderStack::Impl::retire_active_swapchain()
  {
    if (!swapchain_) {
      return;
    }
    retired_swapchains_.push_back(std::move(*swapchain_));
    swapchain_.reset();
  }

  std::optional<std::uint32_t> WindowsRenderStack::Impl::find_memory_type(
    const std::uint32_t bits, const VkMemoryPropertyFlags required, const VkMemoryPropertyFlags preferred) const noexcept
  {
    return memory_type_index(memory_properties_, bits, required, preferred);
  }

  std::expected<void, core::AppError>
  WindowsRenderStack::Impl::create_buffer_memory(CaptureSlot &slot,
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
  WindowsRenderStack::Impl::create_image_memory(CaptureSlot &slot)
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
  WindowsRenderStack::Impl::recreate_render_finished(CaptureSlot &slot)
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
  WindowsRenderStack::Impl::recreate_acquire_semaphore(CaptureSlot &slot)
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
  WindowsRenderStack::Impl::create_slot_presentation_resources(
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
  WindowsRenderStack::Impl::create_capture_slot(CaptureSlot &slot)
  {
    D3D11_TEXTURE2D_DESC texture_info{};
    texture_info.Width = capture_allocation_.width;
    texture_info.Height = capture_allocation_.height;
    texture_info.MipLevels = 1;
    texture_info.ArraySize = 1;
    texture_info.Format = capture_format_;
    texture_info.SampleDesc.Count = 1;
    texture_info.Usage = D3D11_USAGE_STAGING;
    texture_info.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    auto d3_result = d3_device_->CreateTexture2D(
      &texture_info, nullptr, slot.readback_texture.GetAddressOf());
    if (FAILED(d3_result)) {
      return std::unexpected{
        d3_error(core::AppErrorCode::RuntimeConstructionFailure, d3_result)};
    }
    D3D11_QUERY_DESC query_info{};
    query_info.Query = D3D11_QUERY_EVENT;
    d3_result =
      d3_device_->CreateQuery(&query_info, slot.copy_query.GetAddressOf());
    if (FAILED(d3_result)) {
      return std::unexpected{
        d3_error(core::AppErrorCode::RuntimeConstructionFailure, d3_result)};
    }

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

  std::expected<bool, core::AppError>
  WindowsRenderStack::Impl::query_interop_slot(CaptureSlot &slot)
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
          .width = capture_allocation_.width,
          .height = capture_allocation_.height,
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
      vkCreateImage(device_, &image_info, nullptr, &created_interop_image);
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
    vkGetImageMemoryRequirements2(device_, &requirements_info, &requirements);

    D3D11_TEXTURE2D_DESC texture_info{};
    texture_info.Width = capture_allocation_.width;
    texture_info.Height = capture_allocation_.height;
    texture_info.MipLevels = 1;
    texture_info.ArraySize = 1;
    texture_info.Format = capture_format_;
    texture_info.SampleDesc.Count = 1;
    texture_info.Usage = D3D11_USAGE_DEFAULT;
    texture_info.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // NT handles select the handle type, not shared storage. Synchronization
    // stays with the existing shared fence; this is not a keyed-mutex texture.
    texture_info.MiscFlags =
      D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    auto d3_result = d3_device_->CreateTexture2D(
      &texture_info, nullptr, slot.interop_texture.GetAddressOf());
    if (FAILED(d3_result)) {
      return std::unexpected{
        d3_error(core::AppErrorCode::RuntimeConstructionFailure, d3_result)};
    }
    D3D11_QUERY_DESC query_info{};
    query_info.Query = D3D11_QUERY_EVENT;
    d3_result =
      d3_device_->CreateQuery(&query_info, slot.copy_query.GetAddressOf());
    if (FAILED(d3_result)) {
      return std::unexpected{
        d3_error(core::AppErrorCode::RuntimeConstructionFailure, d3_result)};
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
      device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
      slot.interop_handle.get(), &handle_properties);
    if (result != VK_SUCCESS) {
      return false;
    }
    // A format-level import capability cannot choose memory for this handle.
    // Both the image requirements and the exported allocation must allow it.
    const auto compatible_bits = requirements.memoryRequirements.memoryTypeBits &
                                 handle_properties.memoryTypeBits;
    const auto memory_index =
      memory_type_index(memory_properties_, compatible_bits, 0,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_index) {
      return false;
    }
    ++interop_compatible_handle_count_;
    slot.interop_memory_requirements = requirements.memoryRequirements;
    slot.interop_memory_type_index = *memory_index;
    slot.interop_requires_dedicated =
      interop_dedicated_only_ ||
      dedicated_requirements.requiresDedicatedAllocation == VK_TRUE;
    return true;
  }

  std::expected<void, core::AppError>
  WindowsRenderStack::Impl::finish_interop_slot(CaptureSlot &slot)
  {
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
    const auto result = vkAllocateMemory(device_, &allocation_info, nullptr,
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
      vkBindImageMemory(device_, slot.interop_image, slot.interop_memory, 0);
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
      vkCreateImageView(device_, &view_info, nullptr, &created_interop_view);
    if (next_result != VK_SUCCESS) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure, next_result)};
    }
    slot.interop_view = created_interop_view;
    if (auto created = create_slot_presentation_resources(
          slot, slot.interop_view, VK_IMAGE_LAYOUT_GENERAL);
        !created) {
      return std::unexpected{created.error()};
    }
    return {};
  }

  void WindowsRenderStack::Impl::destroy_interop_slot(
    CaptureSlot &slot) noexcept
  {
    slot.interop_handle.reset();
    if (device_ != VK_NULL_HANDLE) {
      if (slot.interop_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, slot.interop_view, nullptr);
      }
      if (slot.interop_image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, slot.interop_image, nullptr);
      }
      if (slot.interop_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, slot.interop_memory, nullptr);
      }
    }
    slot.interop_texture.Reset();
    slot.interop_view = VK_NULL_HANDLE;
    slot.interop_image = VK_NULL_HANDLE;
    slot.interop_memory = VK_NULL_HANDLE;
    slot.d3_fence_target = 0;
    slot.gpu_copy = false;
    slot.query_completion = false;
  }

  void WindowsRenderStack::Impl::destroy_capture_slot(
    CaptureSlot &slot) noexcept
  {
    // Pools span capture allocations. Preserve the command buffer for the next
    // ring while clearing every allocation-local owner and borrowed mapping.
    const auto command_buffer = slot.command_buffer;
    slot.frame.reset();
    slot.copy_query.Reset();
    slot.readback_texture.Reset();
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
      destroy_interop_slot(slot);
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

  void WindowsRenderStack::Impl::destroy_capture_slots() noexcept
  {
    retained_slot_ = nullptr;
    for (auto &slot : slots_) {
      destroy_capture_slot(slot);
    }
    capture_allocation_ = {};
    capture_format_ = DXGI_FORMAT_UNKNOWN;
  }

  std::expected<void, core::AppError>
  WindowsRenderStack::Impl::rebuild_capture_slots(const std::uint64_t generation,
                                                  const core::Extent allocation,
                                                  const DXGI_FORMAT format)
  {
    if (generation == 0 || !checked_pixel_bytes(allocation) ||
        format != DXGI_FORMAT_B8G8R8A8_UNORM || !render_allocation_idle() ||
        vulkan_device_lost_ || d3_device_removed_) {
      return std::unexpected{graphics_error(core::AppErrorCode::InvalidValue)};
    }
    const bool generation_changed = generation != generation_;
    invalidate_presentation();
    for (auto &slot : slots_) {
      if (slot.phase == SlotPhase::CpuReady ||
          slot.phase == SlotPhase::GpuReady) {
        slot.phase = SlotPhase::Empty;
        slot.gpu_copy = false;
        slot.d3_fence_target = 0;
        ++dropped_frames_;
        observe_performance(PerformanceCounter::Dropped);
        progress_pending_ = true;
      }
    }
    // The old ring is discarded before constructing replacements. The caller's
    // capture barrier must remain closed on failure; there is no old-ring rollback.
    destroy_capture_slots();
    const auto reset_result = vkResetDescriptorPool(device_, descriptor_pool_, 0);
    if (reset_result != VK_SUCCESS) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure, reset_result)};
    }
    capture_allocation_ = allocation;
    capture_format_ = format;

    const auto create_cpu_ring = [this]() -> std::expected<void, core::AppError> {
      for (auto &slot : slots_) {
        if (auto created = create_capture_slot(slot); !created) {
          const auto error = created.error();
          destroy_capture_slots();
          return std::unexpected{error};
        }
      }
      return {};
    };

    const auto interop_prefix_available = [this]() noexcept {
      return bridge_capabilities_collected_ &&
             bridge_facts_.adapter_luid_matches &&
             bridge_facts_.external_image_supported &&
             bridge_facts_.required_d3d11_interfaces_available &&
             bridge_facts_.shared_fence_or_timeline_semaphore_available &&
             bridge_facts_.timeline_fence_import_available &&
             bridge_facts_.win32_import_procedure_available;
    };
    const bool cpu_fallback_selected =
      bridge_selection_ && bridge_selection_->mode == core::BridgeMode::CpuCopy;
    if (generation_changed && cpu_fallback_selected &&
        interop_prefix_available()) {
      // CPU fallback releases generation-local D3 interop objects. Recreate
      // those capability objects before probing the next generation's handles.
      collect_bridge_capabilities();
    }
    const bool probe_interop = should_probe_interop_generation(
      interop_prefix_available(), bridge_selection_, generation_, generation);
    const bool attempt_interop =
      bridge_preference_ == WindowsBridgePreference::Automatic && probe_interop;

    bool interop_operation_failed{};
    if (probe_interop) {
      interop_created_handle_count_ = 0;
      interop_compatible_handle_count_ = 0;
      for (auto &slot : slots_) {
        const auto queried = query_interop_slot(slot);
        if (!queried) {
          interop_operation_failed = true;
          break;
        }
        if (!*queried) {
          bridge_facts_.compatible_memory_type_available = false;
          break;
        }
      }
      bridge_facts_.compatible_memory_type_available =
        interop_created_handle_count_ != 0 &&
        interop_created_handle_count_ == interop_compatible_handle_count_;
      if (attempt_interop && bridge_facts_.compatible_memory_type_available &&
          !interop_operation_failed) {
        for (auto &slot : slots_) {
          if (auto finished = finish_interop_slot(slot); !finished) {
            interop_operation_failed = true;
            break;
          }
        }
      }
      if (attempt_interop && bridge_facts_.compatible_memory_type_available &&
          !interop_operation_failed && !interop_timeline_ready_ &&
          !create_interop_timeline()) {
        interop_operation_failed = true;
      }
    }
    else if (!bridge_selection_) {
      bridge_facts_.compatible_memory_type_available = false;
    }

    if (attempt_interop && bridge_facts_.compatible_memory_type_available &&
        !interop_operation_failed) {
      active_bridge_mode_ = core::BridgeMode::GpuInterop;
      const auto selected = select_bridge(bridge_facts_);
      if (!bridge_selection_ || *bridge_selection_ != selected) {
        bridge_change_pending_ = selected;
      }
      bridge_selection_ = selected;
    }
    else {
      destroy_capture_slots();
      const auto descriptor_reset =
        vkResetDescriptorPool(device_, descriptor_pool_, 0);
      if (descriptor_reset != VK_SUCCESS) {
        return std::unexpected{vulkan_error(
          core::AppErrorCode::RuntimeConstructionFailure, descriptor_reset)};
      }
      capture_allocation_ = allocation;
      capture_format_ = format;
      if (auto created = create_cpu_ring(); !created) {
        return created;
      }
      if (interop_timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, interop_timeline_, nullptr);
        observe_performance(PerformanceCounter::SemaphoresDestroyed);
        interop_timeline_ = VK_NULL_HANDLE;
      }
      interop_timeline_ready_ = false;
      interop_fence_.Reset();
      d3_context4_.Reset();
      d3_device5_.Reset();
      active_bridge_mode_ = core::BridgeMode::CpuCopy;
      if (bridge_preference_ == WindowsBridgePreference::Automatic &&
          (!bridge_selection_ ||
           bridge_selection_->mode == core::BridgeMode::GpuInterop ||
           generation_changed)) {
        const auto capability = select_bridge(bridge_facts_);
        const auto selected =
          interop_operation_failed ||
              capability.mode == core::BridgeMode::GpuInterop
            ? *select_interop_fallback(
                InteropFallbackPoint::PreSubmitSetupFailure)
            : capability;
        if (!bridge_selection_ || *bridge_selection_ != selected) {
          bridge_change_pending_ = selected;
        }
        bridge_selection_ = selected;
      }
      else if (bridge_preference_ ==
               WindowsBridgePreference::ForceCpuBaseline) {
        const auto capability = select_bridge(bridge_facts_);
        // Case 14 forces the concrete pre-submit fallback after the real
        // capability probes. Unsupported hardware keeps its exact first-failed
        // reason; capable hardware uses the existing operational-fallback reason.
        const auto selected =
          interop_operation_failed ||
              capability.mode == core::BridgeMode::GpuInterop
            ? *select_interop_fallback(
                InteropFallbackPoint::PreSubmitSetupFailure)
            : capability;
        if (!bridge_selection_ || *bridge_selection_ != selected) {
          bridge_change_pending_ = selected;
        }
        bridge_selection_ = selected;
      }
    }
    generation_ = generation;
    return {};
  }

  bool WindowsRenderStack::Impl::ready_for_frame() const noexcept
  {
    return accepting_frames_ && !terminal_error_ && !d3_device_removed_ &&
           capture_format_ != DXGI_FORMAT_UNKNOWN &&
           std::ranges::any_of(slots_, [](const CaptureSlot &slot) noexcept {
             return slot.phase == SlotPhase::Empty;
           });
  }

  WindowsRenderStack::Impl::CaptureSlot *
  WindowsRenderStack::Impl::empty_slot() noexcept
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

  void WindowsRenderStack::Impl::submit(
    platform::windows::WindowsCapturedFrame &&frame)
  {
    auto *const slot = empty_slot();
    const auto content = frame.content_size();
    if (!ready_for_frame() || slot == nullptr ||
        frame.resource_generation() != generation_ ||
        frame.allocation_extent() != capture_allocation_ ||
        frame.expected_pool_format() !=
          winrt::Windows::Graphics::DirectX::DirectXPixelFormat::
            B8G8R8A8UIntNormalized ||
        has_zero_dimension(content) ||
        content.width > capture_allocation_.width ||
        content.height > capture_allocation_.height) {
      ++dropped_frames_;
      observe_performance(PerformanceCounter::Dropped);
      throw graphics_error(core::AppErrorCode::InvalidValue);
    }

    auto source_texture = frame.texture();
    D3D11_TEXTURE2D_DESC source_info{};
    source_texture->GetDesc(&source_info);
    ComPtr<ID3D11Device> source_device;
    source_texture->GetDevice(source_device.GetAddressOf());
    if (source_device.Get() != d3_device_.Get() ||
        source_info.Width < content.width ||
        source_info.Height < content.height ||
        source_info.Format != capture_format_) {
      ++dropped_frames_;
      observe_performance(PerformanceCounter::Dropped);
      throw graphics_error(core::AppErrorCode::InvalidValue);
    }

    const D3D11_BOX source_box{
      .left = 0,
      .top = 0,
      .front = 0,
      .right = content.width,
      .bottom = content.height,
      .back = 1,
    };
    if (slot == retained_slot_) {
      retained_slot_ = nullptr;
    }
    // Keep the pool checkout alive across asynchronous D3 access. A texture
    // interface alone would not prevent WGC from recycling its pool buffer.
    slot->frame.emplace(std::move(frame));
    slot->stamp.intake_observed_at = slot->frame->capture_timestamp();
    slot->content = content;
    performance_content_ = content;
    slot->gpu_copy = active_bridge_mode_ == core::BridgeMode::GpuInterop;
    if (slot->gpu_copy) {
      if (next_d3_fence_target_ == (std::numeric_limits<std::uint64_t>::max)()) {
        slot->frame.reset();
        slot->gpu_copy = false;
        ++dropped_frames_;
        observe_performance(PerformanceCounter::Dropped);
        request_interop_fallback();
        return;
      }
      slot->d3_fence_target = next_d3_fence_target_++;
      d3_context_->CopySubresourceRegion(slot->interop_texture.Get(), 0, 0, 0, 0,
                                         source_texture.Get(), 0, &source_box);
      observe_performance(PerformanceCounter::D3CopyBytes, static_cast<std::uint64_t>(content.width) * content.height * bytes_per_pixel);
      const auto signal_result =
        d3_context4_->Signal(interop_fence_.Get(), slot->d3_fence_target);
      if (FAILED(signal_result)) {
        // Copy was issued before Signal failed. A healthy device still owes copy
        // completion, so append a query and retain the checkout while falling back.
        const auto removal = d3_device_->GetDeviceRemovedReason();
        if (FAILED(removal)) {
          d3_device_removed_ = true;
          accepting_frames_ = false;
          slot->frame.reset();
          slot->phase = SlotPhase::Empty;
          ++dropped_frames_;
          observe_performance(PerformanceCounter::Dropped);
          return;
        }
        d3_context_->End(slot->copy_query.Get());
        d3_context_->Flush();
        slot->query_completion = true;
        request_interop_fallback();
      }
      else {
        d3_context_->Flush();
      }
    }
    else {
      d3_context_->CopySubresourceRegion(slot->readback_texture.Get(), 0, 0, 0, 0,
                                         source_texture.Get(), 0, &source_box);
      observe_performance(PerformanceCounter::D3CopyBytes, static_cast<std::uint64_t>(content.width) * content.height * bytes_per_pixel);
      d3_context_->End(slot->copy_query.Get());
      d3_context_->Flush();
    }
    slot->phase = SlotPhase::ProducerPending;
    ++captured_frames_;
    observe_performance(PerformanceCounter::Admitted);
    slot->stamp.sequence = captured_frames_;
  }

  bool WindowsRenderStack::Impl::has_unsafe_native_work() const noexcept
  {
    // Test D3 before the Vulkan-loss shortcut: loss of one API does not discharge
    // the source-pool lease held by a healthy asynchronous copy on the other.
    if (!d3_device_removed_ &&
        std::ranges::any_of(slots_, [](const CaptureSlot &slot) noexcept {
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

  SwapchainOwnershipFacts WindowsRenderStack::Impl::swapchain_ownership(
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

  std::size_t WindowsRenderStack::Impl::slots_in_flight() const noexcept
  {
    return static_cast<std::size_t>(
      std::ranges::count_if(slots_, [](const CaptureSlot &slot) noexcept {
        return slot.phase != SlotPhase::Empty;
      }));
  }

  std::size_t WindowsRenderStack::Impl::presentations_in_flight() const noexcept
  {
    return static_cast<std::size_t>(std::ranges::count_if(
      present_records_,
      [](const std::optional<PresentRecord> &record) noexcept {
        return record.has_value();
      }));
  }

  bool WindowsRenderStack::Impl::render_allocation_idle() const noexcept
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

  WindowsRenderStack::Impl::CaptureSlot *
  WindowsRenderStack::Impl::ready_slot() noexcept
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
        // D3 completion has already checked in the WGC frame, and Vulkan has
        // not used this slot. Dropping stale ready content needs no native wait.
        slot.phase = SlotPhase::Empty;
        ++dropped_frames_;
        observe_performance(PerformanceCounter::Dropped);
      }
    }
    return newest;
  }

  WindowsRenderStack::Impl::PresentRecord *
  WindowsRenderStack::Impl::empty_present_record() noexcept
  {
    const auto found = std::ranges::find_if(
      present_records_,
      [](const std::optional<PresentRecord> &record) { return !record; });
    if (found == present_records_.end()) {
      return nullptr;
    }
    return &found->emplace();
  }

  void WindowsRenderStack::Impl::destroy_present_records() noexcept
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

  void WindowsRenderStack::Impl::request_interop_fallback() noexcept
  {
    if (active_bridge_mode_ != core::BridgeMode::GpuInterop ||
        interop_fallback_pending_) {
      return;
    }
    accepting_frames_ = false;
    interop_fallback_pending_ = true;
  }

  void WindowsRenderStack::Impl::progress_interop_fallback(
    RenderTickResult &result)
  {
    if (!interop_fallback_pending_ || vulkan_device_lost_ || d3_device_removed_) {
      return;
    }
    for (auto &slot : slots_) {
      if (slot.phase == SlotPhase::GpuReady) {
        slot.phase = SlotPhase::Empty;
        slot.gpu_copy = false;
        slot.d3_fence_target = 0;
        ++dropped_frames_;
        observe_performance(PerformanceCounter::Dropped);
        result.made_progress = true;
      }
    }
    const auto fallback = select_interop_fallback(
      slots_in_flight() == 0 && presentations_in_flight() == 0
        ? InteropFallbackPoint::RuntimeWorkRetired
        : InteropFallbackPoint::RuntimeWorkPending);
    if (!fallback) {
      return;
    }

    // Runtime fallback changes both transport allocations and their shared
    // timeline. Keep the existing owners intact until every tracked user retires;
    // the new CPU ring remains within the same resource generation.
    const auto allocation = capture_allocation_;
    const auto format = capture_format_;
    destroy_capture_slots();
    const auto reset_result = vkResetDescriptorPool(device_, descriptor_pool_, 0);
    if (reset_result != VK_SUCCESS) {
      interop_fallback_pending_ = false;
      throw vulkan_error(core::AppErrorCode::RuntimeStateLost, reset_result);
    }
    capture_allocation_ = allocation;
    capture_format_ = format;
    for (auto &slot : slots_) {
      if (auto created = create_capture_slot(slot); !created) {
        const auto failure = created.error();
        destroy_capture_slots();
        interop_fallback_pending_ = false;
        const bool d3_lost = FAILED(d3_device_->GetDeviceRemovedReason());
        const bool vulkan_lost =
          failure.native_detail &&
          *failure.native_detail ==
            static_cast<std::int64_t>(VK_ERROR_DEVICE_LOST);
        if (d3_lost) {
          mark_d3_device_removed(result);
        }
        if (vulkan_lost) {
          mark_vulkan_device_lost(result);
        }
        if (d3_lost || vulkan_lost) {
          return;
        }
        throw core::AppError{
          .code = core::AppErrorCode::RuntimeStateLost,
          .native_detail = failure.native_detail,
        };
      }
    }
    if (interop_timeline_ != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, interop_timeline_, nullptr);
      observe_performance(PerformanceCounter::SemaphoresDestroyed);
      interop_timeline_ = VK_NULL_HANDLE;
    }
    interop_timeline_ready_ = false;
    interop_fence_.Reset();
    d3_context4_.Reset();
    d3_device5_.Reset();
    active_bridge_mode_ = core::BridgeMode::CpuCopy;
    if (!bridge_selection_ || *bridge_selection_ != *fallback) {
      bridge_change_pending_ = *fallback;
    }
    bridge_selection_ = *fallback;
    interop_fallback_pending_ = false;
    accepting_frames_ = !draining_ && !terminal_error_;
    result.made_progress = true;
  }

  void WindowsRenderStack::Impl::set_presentation(
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

  bool WindowsRenderStack::Impl::has_redraw_work() const noexcept
  {
    return presentation_dirty_ && retained_slot_ && !draining_ &&
           !terminal_error_ && !vulkan_device_lost_ && !d3_device_removed_ &&
           !has_zero_dimension(framebuffer_);
  }

  void WindowsRenderStack::Impl::invalidate_presentation() noexcept
  {
    published_presentation_.reset();
    first_valid_present_sequence_ = next_present_sequence_;
  }

  void WindowsRenderStack::Impl::resize(const core::Extent framebuffer) noexcept
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

  void WindowsRenderStack::Impl::begin_drain() noexcept
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

  void WindowsRenderStack::Impl::begin_device_recovery_drain(
    const DeviceLossKind kind) noexcept
  {
    invalidate_presentation();
    accepting_frames_ = false;
    draining_ = true;
    retained_slot_ = nullptr;
    redraw_requested_ = false;
    if (kind == DeviceLossKind::VulkanDeviceLost) {
      vulkan_device_lost_ = true;
      progress_pending_ = progress_pending_ || presentations_in_flight() != 0;
      destroy_present_records();
    }
    else {
      d3_device_removed_ = true;
    }

    for (auto &slot : slots_) {
      if (plan_device_recovery_slot(kind, slot.phase) !=
          DeviceRecoverySlotAction::ClearWithoutWait) {
        continue;
      }
      progress_pending_ = progress_pending_ || slot.phase != SlotPhase::Empty ||
                          slot.frame.has_value() || slot.acquired_image;
      slot.frame.reset();
      slot.phase = SlotPhase::Empty;
      slot.acquired_image = false;
    }
  }

  void WindowsRenderStack::Impl::mark_vulkan_device_lost(
    RenderTickResult &result) noexcept
  {
    vulkan_device_lost_ = true;
    accepting_frames_ = false;
    result.vulkan_device_lost = true;
    result.device_loss = DeviceLossKind::VulkanDeviceLost;
  }

  void WindowsRenderStack::Impl::mark_d3_device_removed(
    RenderTickResult &result) noexcept
  {
    d3_device_removed_ = true;
    accepting_frames_ = false;
    result.d3_device_removed = true;
    result.device_loss = DeviceLossKind::D3DeviceRemoved;
  }

  void WindowsRenderStack::Impl::set_terminal(
    const VkResult result, RenderTickResult &tick_result) noexcept
  {
    terminal_error_ = vulkan_error(core::AppErrorCode::RuntimeStateLost, result);
    accepting_frames_ = false;
    tick_result.terminal_error = terminal_error_;
  }

  void WindowsRenderStack::Impl::schedule_poll(
    RenderTickResult &result,
    const std::chrono::steady_clock::time_point now) const noexcept
  {
    earlier_deadline(result.next_deadline, now + pending_poll_interval);
  }

  void WindowsRenderStack::Impl::progress_present_records(
    RenderTickResult &result, const std::chrono::steady_clock::time_point now)
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

  void WindowsRenderStack::Impl::progress_d3_copy(
    CaptureSlot &slot, RenderTickResult &result, const std::chrono::steady_clock::time_point now)
  {
    if (d3_device_removed_) {
      slot.frame.reset();
      slot.phase = SlotPhase::Empty;
      result.made_progress = true;
      return;
    }

    if (slot.gpu_copy && !slot.query_completion) {
      const auto completion = classify_d3_completed_value(
        interop_fence_->GetCompletedValue(), slot.d3_fence_target);
      if (completion == D3CompletionStatus::Pending) {
        schedule_poll(result, now);
        return;
      }
      if (completion == D3CompletionStatus::DeviceRemoved) {
        mark_d3_device_removed(result);
        slot.frame.reset();
        slot.phase = SlotPhase::Empty;
        result.made_progress = true;
        return;
      }
      slot.frame.reset();
      result.made_progress = true;
      if (draining_ || vulkan_device_lost_ || interop_fallback_pending_) {
        slot.phase = SlotPhase::Empty;
        ++dropped_frames_;
        observe_performance(PerformanceCounter::Dropped);
        return;
      }
      slot.phase = SlotPhase::GpuReady;
      return;
    }

    BOOL complete{};
    const auto query_result =
      d3_context_->GetData(slot.copy_query.Get(), &complete, sizeof(complete),
                           D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (query_result == S_FALSE || (query_result == S_OK && complete == FALSE)) {
      schedule_poll(result, now);
      return;
    }
    if (FAILED(query_result)) {
      const auto removal = d3_device_->GetDeviceRemovedReason();
      if (FAILED(removal)) {
        mark_d3_device_removed(result);
        slot.frame.reset();
        slot.phase = SlotPhase::Empty;
        result.made_progress = true;
      }
      else {
        terminal_error_ =
          d3_error(core::AppErrorCode::RuntimeStateLost, query_result);
        accepting_frames_ = false;
        result.terminal_error = terminal_error_;
      }
      return;
    }

    // The event query follows CopySubresourceRegion, so the WGC checkout is no
    // longer referenced even if the staging texture is not yet host-mappable.
    if (slot.frame) {
      slot.frame.reset();
      result.made_progress = true;
    }
    if (slot.gpu_copy) {
      slot.phase = SlotPhase::Empty;
      slot.gpu_copy = false;
      slot.query_completion = false;
      ++dropped_frames_;
      observe_performance(PerformanceCounter::Dropped);
      result.made_progress = true;
      return;
    }
    if (draining_ || vulkan_device_lost_) {
      slot.phase = SlotPhase::Empty;
      ++dropped_frames_;
      observe_performance(PerformanceCounter::Dropped);
      result.made_progress = true;
      return;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto map_result =
      d3_context_->Map(slot.readback_texture.Get(), 0, D3D11_MAP_READ,
                       D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (map_result == DXGI_ERROR_WAS_STILL_DRAWING) {
      schedule_poll(result, now);
      return;
    }
    if (FAILED(map_result)) {
      const auto removal = d3_device_->GetDeviceRemovedReason();
      if (FAILED(removal)) {
        mark_d3_device_removed(result);
        slot.phase = SlotPhase::Empty;
        result.made_progress = true;
      }
      else {
        terminal_error_ =
          d3_error(core::AppErrorCode::RuntimeStateLost, map_result);
        accepting_frames_ = false;
        result.terminal_error = terminal_error_;
        slot.phase = SlotPhase::Empty;
        result.made_progress = true;
      }
      return;
    }

    const auto row_bytes =
      static_cast<std::size_t>(slot.content.width) * bytes_per_pixel;
    const auto byte_count = checked_pixel_bytes(capture_allocation_);
    if (!byte_count || mapped.RowPitch < row_bytes || mapped.pData == nullptr) {
      d3_context_->Unmap(slot.readback_texture.Get(), 0);
      terminal_error_ = graphics_error(core::AppErrorCode::RuntimeStateLost);
      accepting_frames_ = false;
      result.terminal_error = terminal_error_;
      slot.phase = SlotPhase::Empty;
      result.made_progress = true;
      return;
    }
    // Upload commands copy the entire allocation, but capture only defines the
    // content rectangle. Clear the remaining bytes and translate D3's row pitch
    // into the tightly packed allocation stride expected by Vulkan's buffer copy.
    std::memset(slot.mapped_upload, 0, static_cast<std::size_t>(*byte_count));
    observe_performance(PerformanceCounter::CpuClearBytes, *byte_count);
    auto *const destination = static_cast<std::byte *>(slot.mapped_upload);
    const auto *const source = static_cast<const std::byte *>(mapped.pData);
    const auto destination_pitch =
      static_cast<std::size_t>(capture_allocation_.width) * bytes_per_pixel;
    for (std::uint32_t row = 0; row < slot.content.height; ++row) {
      std::memcpy(destination + static_cast<std::size_t>(row) * destination_pitch,
                  source + static_cast<std::size_t>(row) * mapped.RowPitch,
                  row_bytes);
    }
    d3_context_->Unmap(slot.readback_texture.Get(), 0);

    observe_performance(PerformanceCounter::CpuCopyBytes, static_cast<std::uint64_t>(row_bytes) * slot.content.height);

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
        mark_vulkan_device_lost(result);
        slot.phase = SlotPhase::Empty;
        result.made_progress = true;
        return;
      }
      if (flush_result != VK_SUCCESS) {
        set_terminal(flush_result, result);
        slot.phase = SlotPhase::Empty;
        result.made_progress = true;
        return;
      }
    }

    slot.phase = SlotPhase::CpuReady;
    result.made_progress = true;
  }

  VkResult WindowsRenderStack::Impl::record_commands(
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
      // D3 completion supplies the timeline ordering, while this barrier acquires
      // the shared image for Vulkan sampling. Both are needed on retained redraw
      // too, since the prior command sequence returned ownership to EXTERNAL.
      const VkImageMemoryBarrier acquire_external{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex = graphics_queue_family_,
        .image = slot.interop_image,
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
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = graphics_queue_family_,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image = slot.interop_image,
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
    // waits for that proof before allowing a later D3 copy to overwrite the image.
    return vkEndCommandBuffer(slot.command_buffer);
  }

  void WindowsRenderStack::Impl::reset_slot_after_vulkan(CaptureSlot &slot)
  {
    // Every caller reaches here only after native use was proven complete (or
    // after the owning swapchain was legally destroyed). A synchronization
    // reset/allocation failure therefore makes the slot unusable, not in flight.
    const bool completed_content = slot.phase == SlotPhase::Submitted;
    slot.acquired_image = false;
    slot.owning_swapchain = VK_NULL_HANDLE;
    slot.phase = SlotPhase::Empty;
    slot.query_completion = false;
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
        !interop_fallback_pending_ &&
        (!retained_slot_ ||
         slot.stamp.sequence >= retained_slot_->stamp.sequence)) {
      // Retain only our completed sampled image, never a WGC checkout. GPU
      // replay reacquires it from EXTERNAL and waits on the same completed D3
      // timeline value; no D3 writer touches this slot before it is reused.
      retained_slot_ = &slot;
    }
  }

  VkResult WindowsRenderStack::Impl::release_swapchain_image(
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

  void WindowsRenderStack::Impl::release_acquired_image(
    CaptureSlot &slot, RenderTickResult &result)
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

  void WindowsRenderStack::Impl::progress_submission(
    CaptureSlot &slot, RenderTickResult &result, const std::chrono::steady_clock::time_point now)
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
    const auto status = vkGetFenceStatus(device_, proof_fence);
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

  void WindowsRenderStack::Impl::progress_slots(
    RenderTickResult &result, const std::chrono::steady_clock::time_point now)
  {
    for (auto &slot : slots_) {
      switch (slot.phase) {
      case SlotPhase::ProducerPending:
        progress_d3_copy(slot, result, now);
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

  void WindowsRenderStack::Impl::try_present(
    CaptureSlot &slot, PresentRecord &record, RenderTickResult &result, const std::chrono::steady_clock::time_point now)
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

    // WSI availability gates attachment writes; shared D3 data gates sampling.
    // The CPU path uses only the acquire wait and has no imported timeline.
    const std::array wait_semaphores{slot.acquire_semaphore, interop_timeline_};
    const std::array<VkPipelineStageFlags, 2> wait_stages{
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
    const std::array<std::uint64_t, 2> wait_values{0, slot.d3_fence_target};
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
    // The D3 timeline target has already completed before this submission is
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

  void WindowsRenderStack::Impl::clear_release_retained_after_swapchain_destroy(
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

  void WindowsRenderStack::Impl::progress_retired_swapchains(
    RenderTickResult &result)
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

  void WindowsRenderStack::Impl::begin_recovery_episode(
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

  void WindowsRenderStack::Impl::note_recovery_failure(
    const core::AppError &error, RenderTickResult &result, const std::chrono::steady_clock::time_point now)
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

  void WindowsRenderStack::Impl::reset_recovery_after_present(
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

  void WindowsRenderStack::Impl::progress_recreation(
    RenderTickResult &result, const std::chrono::steady_clock::time_point now)
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
    const auto scope = graphics_recovery_scope(category);
    if (!render_stack_local_recovery(scope)) {
      return;
    }
    if (!surface_recovery_pending_ && !swapchain_recreate_pending_ &&
        swapchain_) {
      return;
    }
    if (scope.recreate_surface && !surface_recovery_pending_) {
      if (!surface_recovery_active_) {
        result.surface_recovery_started = true;
      }
      surface_recovery_active_ = true;
      surface_recovery_pending_ = true;
      swapchain_recreate_pending_ = false;
    }
    begin_recovery_episode(category, now);

    if (scope.recreate_surface) {
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

    if (scope.recreate_surface) {
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

    if (scope.recreate_surface) {
      surface_recovery_pending_ = false;
      surface_recovery_active_ = false;
      result.surface_recovery_completed = true;
    }
    progress_retired_swapchains(result);
  }

  RenderTickResult WindowsRenderStack::Impl::tick(
    const core::Extent framebuffer,
    const std::chrono::steady_clock::time_point now) noexcept
  {
    RenderTickResult result;
    try {
      result.made_progress = std::exchange(progress_pending_, false);
      if (framebuffer != framebuffer_) {
        resize(framebuffer);
      }
      if (performance_) observe_performance_metadata(std::chrono::steady_clock::now());
      result.presentation_suspended = has_zero_dimension(framebuffer_);
      result.vulkan_device_lost = vulkan_device_lost_;
      result.d3_device_removed = d3_device_removed_;
      if (vulkan_device_lost_) {
        result.device_loss = DeviceLossKind::VulkanDeviceLost;
      }
      else if (d3_device_removed_) {
        result.device_loss = DeviceLossKind::D3DeviceRemoved;
      }
      result.terminal_error = terminal_error_;

      // Retire existing owners before considering new work. These passes remain
      // active for zero-size framebuffers, terminal errors, and closed admission.
      progress_present_records(result, now);
      progress_slots(result, now);
      for (auto &record : present_records_) {
        if (record && record->fence == VK_NULL_HANDLE) {
          record.reset();
        }
      }
      progress_interop_fallback(result);

      if (!vulkan_device_lost_) {
        progress_recreation(result, now);
      }

      if (!draining_ && !terminal_error_ && !vulkan_device_lost_ &&
          !interop_fallback_pending_ && !surface_recovery_pending_ &&
          !swapchain_recreate_pending_ && !has_zero_dimension(framebuffer_) &&
          swapchain_) {
        // At most one acquire/submit/present attempt per tick, using a free bounded
        // present record. A full record set supplies its own future poll deadline.
        if (auto *const slot = ready_slot()) {
          if (auto *const record = empty_present_record()) {
            try_present(*slot, *record, result, now);
            if (record->fence == VK_NULL_HANDLE) {
              const auto found = std::ranges::find_if(
                present_records_, [record](const auto &candidate) noexcept {
                  return candidate && &*candidate == record;
                });
              if (found != present_records_.end()) {
                found->reset();
              }
            }
          }
        }
      }
    } catch (const core::AppError &error) {
      if (error.native_detail ==
          static_cast<std::int64_t>(VK_ERROR_DEVICE_LOST)) {
        mark_vulkan_device_lost(result);
      }
      else {
        terminal_error_ = error;
        accepting_frames_ = false;
        result.terminal_error = error;
      }
    } catch (const std::bad_alloc &) {
      terminal_error_ =
        graphics_error(core::AppErrorCode::RuntimeStateLost, E_OUTOFMEMORY);
      accepting_frames_ = false;
      result.terminal_error = terminal_error_;
    } catch (...) {
      terminal_error_ = graphics_error(core::AppErrorCode::RuntimeStateLost);
      accepting_frames_ = false;
      result.terminal_error = terminal_error_;
    }

    if (has_unsafe_native_work()) {
      schedule_poll(result, now);
    }
    // This is graphics-owner drain proof only. Capture callback/checkout barriers
    // are combined with it by the caller before releasing the whole runtime.
    result.drain_complete =
      draining_ && slots_in_flight() == 0 && presentations_in_flight() == 0;
    result.cleanup_pending =
      interop_fallback_pending_ || surface_recovery_pending_ ||
      swapchain_recreate_pending_ || !retired_swapchains_.empty();
    result.captured_frames = captured_frames_;
    result.presented_frames = presented_frames_;
    result.dropped_frames = dropped_frames_;
    result.slots_in_flight = slots_in_flight();
    result.presentations_in_flight = presentations_in_flight();
    if (performance_) {
      // Retirement observers run while their records still occupy a container.
      // Publish the final state as well, so a later measurement boundary uses
      // the current occupancy after the last pending record has been erased.
      performance_live_.slots = result.slots_in_flight;
      performance_live_.presents = result.presentations_in_flight;
      performance_->observe_high_water(PerformanceOwner::Replica, performance_live_, std::chrono::steady_clock::now());
    }
    result.vulkan_device_lost = vulkan_device_lost_;
    result.d3_device_removed = d3_device_removed_;
    if (vulkan_device_lost_) {
      result.device_loss = DeviceLossKind::VulkanDeviceLost;
    }
    else if (d3_device_removed_) {
      result.device_loss = DeviceLossKind::D3DeviceRemoved;
    }
    else {
      result.device_loss.reset();
    }
    if (draining_ || terminal_error_ || vulkan_device_lost_ || d3_device_removed_ ||
        surface_recovery_pending_ || swapchain_recreate_pending_ ||
        has_zero_dimension(framebuffer_)) {
      invalidate_presentation();
    }
    result.presentation = published_presentation_;
    return result;
  }

  WindowsRenderStack::WindowsRenderStack(
    platform::windows::OverlayWindow &overlay,
    const std::uint64_t resource_generation,
    const WindowsBridgePreference bridge_preference,
    diagnostics::PerformanceReport *performance, const std::uint64_t observation_instance)
      : implementation_{std::make_unique<Impl>(
          overlay.native_window(), resource_generation, overlay.framebuffer_extent(), bridge_preference, performance, observation_instance)} {}

  void WindowsRenderStack::set_performance_fps(const core::FpsCap fps) noexcept
  {
    implementation_->set_performance_fps(fps);
  }

  WindowsRenderStack::~WindowsRenderStack() noexcept = default;

  ID3D11Device &WindowsRenderStack::capture_device() noexcept
  {
    return implementation_->capture_device();
  }

  bool WindowsRenderStack::ready_for_frame() const noexcept
  {
    return implementation_->ready_for_frame();
  }

  void WindowsRenderStack::set_presentation(const ReplicaPresentation presentation) noexcept
  {
    implementation_->set_presentation(presentation);
  }

  bool WindowsRenderStack::has_redraw_work() const noexcept
  {
    return implementation_->has_redraw_work();
  }

  void WindowsRenderStack::request_redraw() noexcept
  {
    implementation_->request_redraw();
  }

  void WindowsRenderStack::submit(
    platform::windows::WindowsCapturedFrame &&frame)
  {
    implementation_->submit(std::move(frame));
  }

  RenderTickResult WindowsRenderStack::tick(
    const core::Extent framebuffer,
    const std::chrono::steady_clock::time_point now) noexcept
  {
    return implementation_->tick(framebuffer, now);
  }

  bool WindowsRenderStack::render_allocation_idle() const noexcept
  {
    return implementation_->render_allocation_idle();
  }

  std::expected<void, core::AppError>
  WindowsRenderStack::rebuild_capture_slots(const std::uint64_t generation,
                                            const core::Extent allocation,
                                            const DXGI_FORMAT format)
  {
    return implementation_->rebuild_capture_slots(generation, allocation, format);
  }

  void WindowsRenderStack::resize(const core::Extent framebuffer) noexcept
  {
    implementation_->resize(framebuffer);
  }

  void WindowsRenderStack::begin_drain() noexcept
  {
    implementation_->begin_drain();
  }

  void WindowsRenderStack::begin_device_recovery_drain(
    const DeviceLossKind kind) noexcept
  {
    implementation_->begin_device_recovery_drain(kind);
  }

  std::uint64_t WindowsRenderStack::generation() const noexcept
  {
    return implementation_->generation();
  }

  BridgeCapabilityFacts
  WindowsRenderStack::bridge_capability_facts() const noexcept
  {
    return implementation_->bridge_capability_facts();
  }

  std::optional<BridgeSelection>
  WindowsRenderStack::bridge_selection() const noexcept
  {
    return implementation_->bridge_selection();
  }

  core::BridgeMode WindowsRenderStack::active_bridge_mode() const noexcept
  {
    return implementation_->active_bridge_mode();
  }

  std::uint64_t
  WindowsRenderStack::gpu_interop_frames_submitted() const noexcept
  {
    return implementation_->gpu_interop_frames_submitted();
  }

  std::optional<BridgeSelection>
  WindowsRenderStack::take_bridge_mode_change() noexcept
  {
    return implementation_->take_bridge_mode_change();
  }

}    // namespace avb::render
