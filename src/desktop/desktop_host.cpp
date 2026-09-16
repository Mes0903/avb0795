/// @file
/// @brief Controller-only presentation, event-loop deadlines and failure containment.
///
/// Each controller submission and presentation retains its own completion proof.
/// The host coordinates these records with application steps without acquiring
/// replica resources. Normal destruction follows proven retirement; uncertain
/// ownership is reported to the composition root for non-unwinding exit.
#include "avb/desktop/desktop_host.hpp"
#include "avb/diagnostics/performance_report.hpp"
#include "controller_font.hpp"
#include "imgui_input_sync.hpp"
#include "ui_cadence.hpp"

#include "avb/ui/controller_view.hpp"
#include "render/graphics_retry_policy.hpp"

#include <vulkan/vulkan.h>

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace avb::desktop {
  using diagnostics::PerformanceCounter;
  using diagnostics::PerformanceOwner;
  using diagnostics::PerformanceSample;
  namespace {

    using namespace std::chrono_literals;

#if defined(VK_KHR_surface_maintenance1) && \
  defined(VK_KHR_swapchain_maintenance1) && \
  defined(VK_KHR_get_surface_capabilities2)
#define AVB0795_DESKTOP_HAS_KHR_MAINTENANCE 1
#endif

#if defined(VK_EXT_surface_maintenance1) && \
  defined(VK_EXT_swapchain_maintenance1) && \
  defined(VK_KHR_get_surface_capabilities2)
#define AVB0795_DESKTOP_HAS_EXT_MAINTENANCE 1
#endif

    constexpr int controller_width{520};
    constexpr int controller_height{720};
    constexpr int controller_failure_exit_code{2};
    constexpr int abandon_exit_code{3};
    constexpr auto active_poll_interval{2ms};
    constexpr auto drain_poll_interval{50ms};
    constexpr auto drain_stalled_threshold{10s};
    constexpr std::array controller_retry_deadlines{0ms, 100ms, 500ms};

    /// Internal C++ exception that exits the backend before it uses failed output.
    struct BackendVulkanFailure final {
      VkResult result;
    };

    /// @brief Exit the C++ backend immediately instead of using failed native outputs.
    ///
    /// Returning to the backend would allow it to continue after a failed call.
    /// Throw only across this C++ callback boundary: the enclosing host catches
    /// the fixed result and preserves possibly referenced graphics resources.
    /// This is not the policy for C-facing window callbacks, which contain errors.
    void check_backend_result(const VkResult result)
    {
      if (result != VK_SUCCESS) {
        throw BackendVulkanFailure{result};
      }
    }

    [[nodiscard]] constexpr core::AppError
    graphics_error(const core::AppErrorCode code,
                   const std::optional<std::int64_t> detail = std::nullopt) noexcept
    {
      return {.code = code, .native_detail = detail};
    }

    [[nodiscard]] constexpr core::AppError
    vulkan_error(const core::AppErrorCode code, const VkResult result) noexcept
    {
      return graphics_error(code, static_cast<std::int64_t>(result));
    }

    /// Preserve the earliest of controller and application progress deadlines.
    template <typename Value>
    void earlier_deadline(std::optional<Value> &current, const Value candidate)
    {
      if (!current || candidate < *current) {
        current = candidate;
      }
    }

    [[nodiscard]] bool extension_available(
      const std::span<const VkExtensionProperties> extensions,
      const char *const required) noexcept
    {
      return std::ranges::any_of(
        extensions, [required](const VkExtensionProperties &extension) noexcept {
          return std::strcmp(extension.extensionName, required) == 0;
        });
    }

    [[nodiscard]] constexpr bool zero_extent(const VkExtent2D extent) noexcept
    {
      return extent.width == 0 || extent.height == 0;
    }

    /// Return one of three episode-relative retry delays, or exhausted budget.
    [[nodiscard]] constexpr std::optional<std::chrono::milliseconds>
    controller_retry_after(
      const std::uint32_t attempt,
      const std::chrono::milliseconds elapsed) noexcept
    {
      if (attempt >= controller_retry_deadlines.size()) {
        return std::nullopt;
      }
      const auto deadline = controller_retry_deadlines[attempt];
      return deadline > elapsed ? deadline - elapsed : 0ms;
    }

    /// Maintenance1 feature and function variant enabled on the controller device.
    enum class MaintenanceVariant { None,
                                    Khr,
                                    Ext };

    /// Proof owed by one controller command record, including failed enqueue and
    /// image-release paths. It is independent of presentation retirement.
    enum class FramePhase {
      Free,
      Ready,
      AcquireProofPending,
      Submitted,
      PresentNotEnqueued,
      SubmitUncertain,
      ReleaseRetained,
    };

    /// Host shutdown mode: uncertainty/device loss preserves controller owners
    /// for non-unwinding exit, while safe WSI/terminal drain can finish normally.
    enum class FatalKind { None,
                           SurfaceLost,
                           DeviceLost,
                           Terminal,
                           Unexpected };

  }    // namespace

  /// @brief Keep controller graphics ownership and its progress loop in one main-thread owner.
  ///
  /// Frame records prove command/acquire completion; present records independently
  /// prove the presentation engine has released its wait semaphore and swapchain.
  /// Permanent records keep native storage; active phases identify outstanding
  /// use. Recycling requires the corresponding independent completion proof.
  /// Domain mutation stays in the application; callbacks emit commands or wakes.
  class DesktopHost::Impl final {
  public:
    /// Permanent command/acquire resources and the current UI frame's ownership.
    /// Present resources are borrowed by identity until presentation takes over.
    struct FrameRecord final {
      VkCommandBuffer command_buffer{VK_NULL_HANDLE};
      VkFence acquire_fence{VK_NULL_HANDLE};
      VkFence submission_fence{VK_NULL_HANDLE};
      VkSemaphore acquire_semaphore{VK_NULL_HANDLE};
      std::optional<render::PresentUseKey> present_use;
      VkSwapchainKHR swapchain{VK_NULL_HANDLE};
      std::uint32_t image_index{};
      FramePhase phase{FramePhase::Free};
      bool acquired{};
      bool uploads_font{};
    };

    /// Retain present fence, consumed wait semaphore and WSI identity until the
    /// presentation engine's completion can be observed on the host.
    struct PresentRecord final {
      render::PresentUseState use;
      VkFence fence{VK_NULL_HANDLE};
      VkSemaphore wait_semaphore{VK_NULL_HANDLE};
      VkSwapchainKHR swapchain{VK_NULL_HANDLE};
      bool confirms_recovery{};
      bool counts_as_presented{};
    };

    /// Active or retired controller WSI allocation, including all image views and
    /// framebuffers that depend on its images. Retiring forbids new acquisition
    /// but does not destroy presentation references. A same-format replacement
    /// keeps the existing ImGui pipeline's render-pass compatibility.
    struct SwapchainState final {
      VkSwapchainKHR handle{VK_NULL_HANDLE};
      VkFormat format{VK_FORMAT_UNDEFINED};
      VkExtent2D extent{};
      VkRenderPass render_pass{VK_NULL_HANDLE};
      std::uint32_t minimum_image_count{2};
      std::vector<VkImage> images;
      std::vector<VkImageView> views;
      std::vector<VkFramebuffer> framebuffers;
      bool recovery_candidate{};
    };

    /// Static atlas resources with two different release conditions. Upload staging
    /// is needed only until the first upload submission is proven complete; the
    /// sampled image/view/descriptor remains referenced by later UI submissions.
    /// The two upload flags distinguish work recorded for a future submit from
    /// work that may already be executing, including an uncertain submit result.
    struct FontTexture final {
      VkBuffer staging_buffer{VK_NULL_HANDLE};
      VkDeviceMemory staging_memory{VK_NULL_HANDLE};
      VkImage image{VK_NULL_HANDLE};
      VkDeviceMemory memory{VK_NULL_HANDLE};
      VkImageView view{VK_NULL_HANDLE};
      VkDescriptorSet descriptor{VK_NULL_HANDLE};
      std::uint32_t width{};
      std::uint32_t height{};
      bool upload_pending{};
      bool upload_in_flight{};
    };

    /// Controller-local retries, reset only by a replacement's retired present.
    struct RecoveryEpisode final {
      render::GraphicsRecoveryEpisodeState state{};
      std::chrono::steady_clock::time_point started_at{};
      std::optional<core::AppError> last_error;
    };

    /// Adopt the controller window and font storage, then build private Vulkan
    /// and ImGui owners without submitting startup work.
    Impl(GLFWwindow *window, CommandSink command_sink, ControllerWindowSetup setup, std::vector<std::byte> font_data,
         diagnostics::PerformanceReport *performance);
    /// Require a drained controller; destruction is not a blocking drain path.
    ~Impl() noexcept;

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    /// Return the platform setup result without re-querying native exclusion.
    [[nodiscard]] core::CaptureExclusionStatus
    controller_exclusion() const noexcept
    {
      return controller_exclusion_;
    }

    /// Post GLFW's thread-safe wake event; callback owners must keep this host
    /// alive until every admitted wake invocation has returned.
    void wake() const noexcept { glfwPostEmptyEvent(); }
    /// Admit recorder input using current native focus and controller lifetime.
    /// The composition root chooses the recovery command when this returns false.
    [[nodiscard]] bool try_record_hotkey(core::HotkeyChord chord) noexcept;
    /// Own the main-thread controller loop and failure firewall; an abandon result
    /// requires the composition root to exit without unwinding native owners.
    [[nodiscard]] HostRunResult run(app::Application &application) noexcept;

  private:
    /// Record native-owner scalars while preserving the ordinary disabled path.
    void observe_performance(PerformanceCounter counter, std::uint64_t value = 1) noexcept;
    void observe_performance_metadata(std::chrono::steady_clock::time_point now) noexcept;
    /// Convert controller close into a portable Quit command once. Contain sink
    /// allocation failure before returning across the GLFW callback boundary.
    static void window_close_callback(GLFWwindow *window) noexcept;
    /// Cancel recording at the focus-loss edge before queued ImGui events coalesce.
    static void window_focus_callback(GLFWwindow *window, int focused) noexcept;
    static void key_callback(GLFWwindow *window, int key, int scancode, int action, int mods) noexcept;
    static void char_callback(GLFWwindow *window, unsigned int character) noexcept;
    static void mouse_button_callback(GLFWwindow *window, int button, int action, int mods) noexcept;
    static void scroll_callback(GLFWwindow *window, double x, double y) noexcept;
    static void cursor_position_callback(GLFWwindow *window, double x, double y) noexcept;
    static void cursor_enter_callback(GLFWwindow *window, int entered) noexcept;
    static void framebuffer_callback(GLFWwindow *window, int width, int height) noexcept;
    static void content_scale_callback(GLFWwindow *window, float x, float y) noexcept;
    static void refresh_callback(GLFWwindow *window) noexcept;
    static void iconify_callback(GLFWwindow *window, int iconified) noexcept;

    /// @brief Observe activity and contain failures across a typed native callback.
    /// Input hooks forward exactly once to the ImGui backend, which owns its saved
    /// predecessor chain. Display hooks forward to their separately saved native
    /// predecessor; a null predecessor still records the activity observation.
    template <typename... Args>
    static void forward_input(GLFWwindow *window,
                              void (*callback)(GLFWwindow *, Args...),
                              Args... args) noexcept
    {
      auto *const self = static_cast<Impl *>(glfwGetWindowUserPointer(window));
      if (self == nullptr) return;
      self->ui_cadence_.note_input(std::chrono::steady_clock::now());
      try {
        if (callback != nullptr) callback(window, args...);
      } catch (...) {
        self->enter_fatal(FatalKind::Unexpected,
                         graphics_error(core::AppErrorCode::RuntimeStateLost));
      }
    }

    /// Observe native drawable state independently of the widget-build deadline.
    /// Windows may retain a small framebuffer while iconified; both facts govern
    /// visibility and swapchain reconstruction admission.
    void observe_controller_visibility(std::chrono::steady_clock::time_point now) noexcept;
    /// Combine public ImGui held-input/editing facts with view-owned gestures.
    [[nodiscard]] bool interaction_active() const noexcept;

    /// Enqueue a view command and mark that the next event wait must be skipped.
    void emit(app::AppCommand command);
    /// Create the controller's own Vulkan instance with GLFW and maintenance1
    /// requirements; no replica graphics object is shared with this owner.
    void create_instance();
    /// Bind the controller surface to the already-created GLFW window.
    void create_surface();
    /// Choose a Vulkan 1.3 presentation device with enabled maintenance1 support.
    void select_physical_device();
    /// Create graphics/present queues and resolve the matching image-release entry.
    void create_device();
    /// Create host-owned command/descriptor pools before backend initialization.
    void create_common_resources();
    /// Build compatible controller WSI and retire active before a replacement
    /// create attempt; keep its presentation resources alive in the retired set.
    [[nodiscard]] std::expected<void, core::AppError>
    create_swapchain(VkExtent2D requested, bool recovery_candidate);
    /// Create candidate image views, compatible render pass and framebuffers;
    /// partial candidate failure remains safe because it has never been submitted.
    [[nodiscard]] std::expected<void, core::AppError>
    create_swapchain_resources(SwapchainState &state);
    /// Create context and both backends, attach the borrowed static font data,
    /// then disable backend texture uploads that would bypass host-owned fences.
    void initialize_imgui();
    /// Bake fixed glyph coverage and create upload/image/descriptor resources.
    /// The first ordinary controller submission uploads the atlas under its fence.
    void create_font_texture();

    /// Build fixed-capacity owners before admitting native work; a partial prefix
    /// remains attached to the host for its existing startup cleanup.
    void create_frame_resources();
    /// Allocate one command buffer and unsignaled acquire/submission sync.
    [[nodiscard]] std::expected<FrameRecord, core::AppError>
    create_frame_record();
    /// Allocate one independent render-finished semaphore and present fence.
    [[nodiscard]] std::expected<PresentRecord, core::AppError>
    create_present_record();
    /// Recycle a proven frame or cancel an unchanged no-image reservation.
    /// Clear use ownership before fallible resets; failure closes admission.
    void recycle_frame(FrameRecord &record) noexcept;
    /// Release one exact use after its proof, retaining the permanent handles.
    [[nodiscard]] bool recycle_present(render::PresentUseKey key,
                                        render::PresentUsePhase phase) noexcept;
    /// Destroy completed signals that no queue wait consumed. Retained image
    /// ownership remains active until release or legal swapchain destruction.
    void discard_unconsumed_signals(FrameRecord &record) noexcept;
    /// Record optional atlas upload and ImGui draw data against the active WSI.
    /// The record tracks font upload so staging survives until submission proof.
    [[nodiscard]] VkResult record_frame(FrameRecord &record,
                                        ImDrawData &draw_data);
    /// Render the application's current immutable view and enqueue emitted commands.
    /// The caller advances the application independently and admits this UI frame
    /// through its cadence policy; native capacity governs the one present attempt.
    void render_controller(app::Application &application,
                           std::chrono::steady_clock::time_point now);
    /// Acquire at timeout zero and independently classify command submit/present.
    /// Reserve bounded frame and present uses before acquiring an image.
    void attempt_present(ImDrawData &draw_data,
                         std::chrono::steady_clock::time_point now);

    /// Poll controller work and WSI transitions; draining disables reconstruction
    /// and drives retirement without a blocking GPU wait.
    void progress(std::chrono::steady_clock::time_point now, bool draining);
    /// Poll each record's acquire/submission proof, retire font staging when safe,
    /// and release acquired images whose presentation never enqueued.
    void progress_frames(std::chrono::steady_clock::time_point now,
                         bool draining);
    /// Poll present fences before releasing their wait semaphores and WSI leases;
    /// only a replacement present's retirement resets its retry episode.
    void progress_presents(std::chrono::steady_clock::time_point now,
                           bool draining);
    /// Use bounded 0/100/500 ms construction attempts after active image use
    /// allows replacement; construction alone does not reset the retry budget.
    void progress_recreation(std::chrono::steady_clock::time_point now);
    /// Destroy retired WSI only after image-use and presentation ownership clears.
    void progress_retired_swapchains();
    /// Release one proven-unused acquired image through the enabled maintenance1 API.
    [[nodiscard]] VkResult release_image(VkSwapchainKHR swapchain,
                                         std::uint32_t image_index) noexcept;
    /// Derive acquired-image, failed-release and present-fence facts for one WSI owner.
    [[nodiscard]] render::SwapchainOwnershipFacts
    ownership(VkSwapchainKHR swapchain) const noexcept;
    /// Check obligations that forbid destructor cleanup; unknown/device-loss host
    /// paths deliberately return abandon rather than unwinding this owner.
    [[nodiscard]] bool unsafe_controller_work() const noexcept;
    /// Require no active/retired WSI or active frame/present use before exit.
    [[nodiscard]] bool controller_drained() const noexcept;
    /// Move active WSI into the retired set; the old handle is never reactivated.
    void retire_active_swapchain();
    /// Clear failed-release records only after legal owning-swapchain destruction.
    void clear_release_retained(VkSwapchainKHR swapchain) noexcept;
    /// Schedule a positive delay: short for active rendering, bounded for drain.
    void schedule_poll(std::chrono::steady_clock::time_point now,
                       bool draining) noexcept;
    /// Reset the controller no-progress clock only after an ownership retirement.
    void note_progress(std::chrono::steady_clock::time_point now) noexcept;
    /// Stop new controller submissions and retain the strongest failure fact.
    /// Application shutdown is driven separately through its public host surface.
    void enter_fatal(FatalKind kind, core::AppError error) noexcept;
    /// Detect ten seconds without retirement while controller owners remain live.
    [[nodiscard]] bool drain_stalled(
      std::chrono::steady_clock::time_point now) const noexcept;

    /// Release proven-complete/never-submitted command and synchronization objects.
    void destroy_frame_record(FrameRecord &record) noexcept;
    /// Destroy a fence and wait semaphore only after presentation retirement proof.
    void destroy_present_record(PresentRecord &record) noexcept;
    /// Release views/render pass/framebuffers and then their safe swapchain owner.
    void destroy_swapchain_state(SwapchainState &state) noexcept;
    /// Release CPU upload storage after the font-upload submission has completed.
    void destroy_font_staging() noexcept;
    /// Destroy the atlas GPU image only after all controller submissions retire.
    void destroy_font_texture() noexcept;
    /// Release resources in dependency order after safe drain or never-submitted
    /// startup failure; this routine performs no queue-idle or fence wait.
    void cleanup_partial() noexcept;

    /// Choose host/device memory by required flags, preferring optional flags.
    [[nodiscard]] std::optional<std::uint32_t>
    memory_type(std::uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) const noexcept;
    /// Read physical framebuffer dimensions; native iconification is observed separately.
    [[nodiscard]] VkExtent2D framebuffer_extent() const noexcept;
    /// Wait for events only until the earliest required observation deadline;
    /// already-due work returns immediately instead of losing its next step.
    void wait_until(std::optional<std::chrono::steady_clock::time_point> deadline,
                    bool may_block) const noexcept;
    /// Keep headless application ticks running after controller failure. The
    /// firewall path preserves controller resources and uses only no-throw host calls.
    void drive_application_shutdown(app::Application &application,
                                    bool controller_firewall);

    GLFWwindow *window_{};
    diagnostics::PerformanceReport *performance_{};
    diagnostics::PerformanceHighWater performance_live_{};
    VkPhysicalDeviceProperties performance_device_{};
    std::array<char, 32> performance_driver_{};
    CommandSink command_sink_;
    core::CaptureExclusionStatus controller_exclusion_{};
    std::unique_ptr<ControllerWindowInteraction> window_interaction_;
    ui::ControllerView controller_view_;
    detail::UiCadence ui_cadence_;
    GLFWframebuffersizefun previous_framebuffer_callback_{};
    GLFWwindowcontentscalefun previous_content_scale_callback_{};
    GLFWwindowrefreshfun previous_refresh_callback_{};
    GLFWwindowiconifyfun previous_iconify_callback_{};
    bool display_callbacks_installed_{};
    // The atlas borrows these bytes; context destruction precedes vector release.
    std::vector<std::byte> font_data_;

    VkInstance instance_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memory_properties_{};
    VkDevice device_{VK_NULL_HANDLE};
    std::uint32_t graphics_queue_family_{};
    std::uint32_t present_queue_family_{};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    VkQueue present_queue_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    MaintenanceVariant maintenance_variant_{MaintenanceVariant::None};
    bool khr_surface_maintenance_enabled_{};
    bool ext_surface_maintenance_enabled_{};
#if defined(AVB0795_DESKTOP_HAS_KHR_MAINTENANCE)
    PFN_vkReleaseSwapchainImagesKHR release_images_khr_{};
#endif
#if defined(AVB0795_DESKTOP_HAS_EXT_MAINTENANCE)
    PFN_vkReleaseSwapchainImagesEXT release_images_ext_{};
#endif

    // These owners remain valid through recoverable WSI replacement and drain.
    std::optional<SwapchainState> active_swapchain_;
    std::vector<SwapchainState> retired_swapchains_;
    std::vector<FrameRecord> frames_;
    std::vector<PresentRecord> presents_;
    std::uint64_t last_present_use_serial_{};
    FontTexture font_texture_;
    std::optional<RecoveryEpisode> recovery_episode_;
    std::optional<std::chrono::steady_clock::time_point> graphics_deadline_;
    std::optional<std::chrono::steady_clock::time_point> no_progress_since_;
    VkFormat imgui_format_{VK_FORMAT_UNDEFINED};
    std::size_t backend_frame_capacity_{2};
    FatalKind fatal_kind_{FatalKind::None};
    bool imgui_context_created_{};
    bool imgui_glfw_initialized_{};
    bool imgui_vulkan_initialized_{};
    bool swapchain_recreate_pending_{};
    bool command_emitted_{};
    bool close_command_emitted_{};
    bool made_progress_{};
    bool stopping_controller_{};
    bool fully_constructed_{};
  };

  void DesktopHost::Impl::observe_performance(const PerformanceCounter counter,
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
    performance_live_.slots = std::ranges::count_if(frames_, [](const auto &record) {
      return record.phase != FramePhase::Free;
    });
    performance_live_.presents = std::ranges::count_if(presents_, [](const auto &record) {
      return record.use.phase != render::PresentUsePhase::Free;
    });
    performance_->observe_counter(PerformanceOwner::Controller, counter, value, now);
    performance_->observe_high_water(PerformanceOwner::Controller, performance_live_, now);
  }

  void DesktopHost::Impl::observe_performance_metadata(
    const std::chrono::steady_clock::time_point now) noexcept
  {
    const auto extent = framebuffer_extent();
    performance_live_.slots = std::ranges::count_if(frames_, [](const auto &record) {
      return record.phase != FramePhase::Free;
    });
    performance_live_.presents = std::ranges::count_if(presents_, [](const auto &record) {
      return record.use.phase != render::PresentUsePhase::Free;
    });
    performance_->observe_metadata(PerformanceOwner::Controller, {
      .gpu = performance_device_.deviceName,
      .driver = performance_driver_.data(),
      .os = "Windows",
      .bridge = "controller",
      .present_mode = "FIFO",
      .ui_policy = "active 16.667ms; idle 100ms; grace 500ms; hidden input-driven; graphics poll 2ms",
      .framebuffer_width = extent.width,
      .framebuffer_height = extent.height,
    }, now);
    performance_->observe_high_water(PerformanceOwner::Controller, performance_live_, now);
  }

  DesktopHost::Impl::Impl(
    GLFWwindow *const window, CommandSink command_sink, ControllerWindowSetup setup, std::vector<std::byte> font_data,
    diagnostics::PerformanceReport *performance)
      : window_{window},
        performance_{performance},
        command_sink_{std::move(command_sink)},
        controller_exclusion_{setup.exclusion},
        window_interaction_{std::move(setup.interaction)},
        font_data_{std::move(font_data)}
  {
    try {
      if (window_ == nullptr || !command_sink_ || !window_interaction_) {
        throw graphics_error(core::AppErrorCode::InvalidValue);
      }
      create_instance();
      create_surface();
      select_physical_device();
      if (performance_) {
        vkGetPhysicalDeviceProperties(physical_device_, &performance_device_);
        std::snprintf(performance_driver_.data(), performance_driver_.size(), "raw:%u", performance_device_.driverVersion);
      }
      create_device();
      create_common_resources();

      const auto initial_extent = framebuffer_extent();
      if (zero_extent(initial_extent)) {
        throw graphics_error(core::AppErrorCode::RuntimeConstructionFailure);
      }
      if (auto created = create_swapchain(initial_extent, false); !created) {
        throw created.error();
      }
      initialize_imgui();
      create_frame_resources();
      create_font_texture();
      glfwSetWindowUserPointer(window_, this);
      glfwSetWindowCloseCallback(window_, window_close_callback);
      // Backend initialization preserves the input predecessor chain. These
      // wrappers add cadence observations; backend shutdown restores that chain.
      glfwSetWindowFocusCallback(window_, window_focus_callback);
      glfwSetKeyCallback(window_, key_callback);
      glfwSetCharCallback(window_, char_callback);
      glfwSetMouseButtonCallback(window_, mouse_button_callback);
      glfwSetScrollCallback(window_, scroll_callback);
      glfwSetCursorPosCallback(window_, cursor_position_callback);
      glfwSetCursorEnterCallback(window_, cursor_enter_callback);
      previous_framebuffer_callback_ = glfwSetFramebufferSizeCallback(window_, framebuffer_callback);
      previous_content_scale_callback_ = glfwSetWindowContentScaleCallback(window_, content_scale_callback);
      previous_refresh_callback_ = glfwSetWindowRefreshCallback(window_, refresh_callback);
      previous_iconify_callback_ = glfwSetWindowIconifyCallback(window_, iconify_callback);
      display_callbacks_installed_ = true;
      fully_constructed_ = true;
    } catch (...) {
      cleanup_partial();
      throw;
    }
  }

  DesktopHost::Impl::~Impl() noexcept
  {
    // Destruction is the end of proven retirement, never an implicit GPU wait.
    if (fully_constructed_ && unsafe_controller_work()) {
      std::terminate();
    }
    cleanup_partial();
    // The input backend restores its previous hook during cleanup_partial().
    // Detach native interaction after that restoration and before window release.
    window_interaction_.reset();
    if (window_ != nullptr) {
      glfwSetWindowCloseCallback(window_, nullptr);
      glfwSetWindowUserPointer(window_, nullptr);
      glfwDestroyWindow(window_);
      window_ = nullptr;
    }
  }

  void DesktopHost::Impl::window_close_callback(GLFWwindow *const window) noexcept
  {
    auto *const self = static_cast<Impl *>(glfwGetWindowUserPointer(window));
    if (self == nullptr || self->close_command_emitted_) {
      return;
    }
    self->close_command_emitted_ = true;
    try {
      self->command_emitted_ =
        self->command_sink_(app::AppCommand{app::Quit{}});
      if (self->command_emitted_ && self->performance_) {
        self->performance_->observe_stop(PerformanceOwner::Controller, std::chrono::steady_clock::now());
      }
    } catch (...) {
      self->enter_fatal(
        FatalKind::Unexpected,
        graphics_error(core::AppErrorCode::RuntimeStateLost));
    }
    glfwPostEmptyEvent();
  }

  void DesktopHost::Impl::window_focus_callback(
    GLFWwindow *const window, const int focused) noexcept
  {
    auto *const self = static_cast<Impl *>(glfwGetWindowUserPointer(window));
    if (self == nullptr) {
      return;
    }
    if (focused == GLFW_FALSE) {
      self->controller_view_.cancel_hotkey_recording();
    }
    forward_input(window, ImGui_ImplGlfw_WindowFocusCallback, focused);
  }

  void DesktopHost::Impl::key_callback(
    GLFWwindow *const window, const int key, const int scancode,
    const int action, const int mods) noexcept
  {
    forward_input(window, ImGui_ImplGlfw_KeyCallback, key, scancode, action, mods);
  }

  void DesktopHost::Impl::char_callback(
    GLFWwindow *const window, const unsigned int character) noexcept
  {
    forward_input(window, ImGui_ImplGlfw_CharCallback, character);
  }

  void DesktopHost::Impl::mouse_button_callback(
    GLFWwindow *const window, const int button, const int action, const int mods) noexcept
  {
    forward_input(window, ImGui_ImplGlfw_MouseButtonCallback, button, action, mods);
  }

  void DesktopHost::Impl::scroll_callback(
    GLFWwindow *const window, const double x, const double y) noexcept
  {
    forward_input(window, ImGui_ImplGlfw_ScrollCallback, x, y);
  }

  void DesktopHost::Impl::cursor_position_callback(
    GLFWwindow *const window, const double x, const double y) noexcept
  {
    forward_input(window, ImGui_ImplGlfw_CursorPosCallback, x, y);
  }

  void DesktopHost::Impl::cursor_enter_callback(
    GLFWwindow *const window, const int entered) noexcept
  {
    forward_input(window, +[](GLFWwindow *target, const int state) {
      ImGui_ImplGlfw_CursorEnterCallback(target, state);
      if (state == GLFW_TRUE) {
        // A leave/enter batch can restore the backend's last consumed frame
        // position while GLFW suppresses a same-position motion callback. Sample
        // the native window-local position at entry so a deferred UI frame sees
        // the current location. Keep this queue write inside forward_input's
        // exception firewall and preserve exactly one backend callback dispatch.
        double x{}, y{};
        glfwGetCursorPos(target, &x, &y);
        ImGui::GetIO().AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
      }
    }, entered);
  }

  void DesktopHost::Impl::framebuffer_callback(
    GLFWwindow *const window, const int width, const int height) noexcept
  {
    if (auto *const self = static_cast<Impl *>(glfwGetWindowUserPointer(window))) {
      forward_input(window, self->previous_framebuffer_callback_, width, height);
    }
  }

  void DesktopHost::Impl::content_scale_callback(
    GLFWwindow *const window, const float x, const float y) noexcept
  {
    if (auto *const self = static_cast<Impl *>(glfwGetWindowUserPointer(window))) {
      forward_input(window, self->previous_content_scale_callback_, x, y);
    }
  }

  void DesktopHost::Impl::refresh_callback(GLFWwindow *const window) noexcept
  {
    if (auto *const self = static_cast<Impl *>(glfwGetWindowUserPointer(window))) {
      forward_input(window, self->previous_refresh_callback_);
    }
  }

  void DesktopHost::Impl::iconify_callback(
    GLFWwindow *const window, const int iconified) noexcept
  {
    if (auto *const self = static_cast<Impl *>(glfwGetWindowUserPointer(window))) {
      forward_input(window, self->previous_iconify_callback_, iconified);
    }
  }

  void DesktopHost::Impl::observe_controller_visibility(
    const std::chrono::steady_clock::time_point now) noexcept
  {
    const auto extent = framebuffer_extent();
    const bool visible = glfwGetWindowAttrib(window_, GLFW_ICONIFIED) != GLFW_TRUE &&
                         !zero_extent(extent);
    ui_cadence_.set_visible(visible, now);
    if (visible && active_swapchain_ &&
        (extent.width != active_swapchain_->extent.width ||
         extent.height != active_swapchain_->extent.height)) {
      swapchain_recreate_pending_ = true;
      schedule_poll(now, false);
    }
  }

  bool DesktopHost::Impl::interaction_active() const noexcept
  {
    if (controller_view_.has_active_interaction() || ImGui::IsAnyItemActive() ||
        ImGui::GetIO().WantTextInput ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                    ImGuiPopupFlags_AnyPopupLevel)) {
      return true;
    }
    for (int button = 0; button < ImGuiMouseButton_COUNT; ++button) {
      if (ImGui::IsMouseDown(button)) return true;
    }
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
      if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key))) return true;
    }
    return false;
  }

  void DesktopHost::Impl::emit(app::AppCommand command)
  {
    const bool closing = std::holds_alternative<app::Quit>(command);
    if (command_sink_(std::move(command))) {
      command_emitted_ = true;
      close_command_emitted_ = close_command_emitted_ || closing;
      if (closing && performance_) {
        performance_->observe_stop(PerformanceOwner::Controller, std::chrono::steady_clock::now());
      }
    }
  }

  bool DesktopHost::Impl::try_record_hotkey(
    const core::HotkeyChord chord) noexcept
  {
    // GLFW reads the native focus now, before the next ImGui frame consumes its
    // queued focus events. This preserves recovery immediately after focus loss.
    const bool ready = fully_constructed_ && window_ != nullptr &&
                       imgui_context_created_ && imgui_glfw_initialized_ &&
                       imgui_vulkan_initialized_ &&
                       ImGui::GetCurrentContext() != nullptr &&
                       !stopping_controller_ && !close_command_emitted_ &&
                       fatal_kind_ == FatalKind::None;
    if (!ready || glfwGetWindowAttrib(window_, GLFW_FOCUSED) != GLFW_TRUE) {
      controller_view_.cancel_hotkey_recording();
      return false;
    }
    const bool recorded = controller_view_.try_record_hotkey(chord);
    if (recorded) ui_cadence_.note_input(std::chrono::steady_clock::now());
    return recorded;
  }

  void DesktopHost::Impl::create_instance()
  {
    if (glfwVulkanSupported() != GLFW_TRUE) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }

    std::uint32_t available_count{};
    auto result = vkEnumerateInstanceExtensionProperties(
      nullptr, &available_count, nullptr);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }
    std::vector<VkExtensionProperties> available(available_count);
    result = vkEnumerateInstanceExtensionProperties(
      nullptr, &available_count, available.data());
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }

    std::uint32_t required_count{};
    const auto *const required =
      glfwGetRequiredInstanceExtensions(&required_count);
    if (required == nullptr || required_count == 0) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }

    std::vector<const char *> enabled;
    enabled.reserve(static_cast<std::size_t>(required_count) + 3);
    const auto append_unique = [&enabled](const char *const name) {
      if (std::ranges::none_of(
            enabled, [name](const char *const current) noexcept {
              return std::strcmp(name, current) == 0;
            })) {
        enabled.push_back(name);
      }
    };
    for (std::uint32_t index{}; index < required_count; ++index) {
      if (!extension_available(available, required[index])) {
        throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
      }
      append_unique(required[index]);
    }

#if defined(VK_KHR_get_surface_capabilities2)
    const bool capabilities2_available = extension_available(
      available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
#else
    constexpr bool capabilities2_available = false;
#endif
#if defined(AVB0795_DESKTOP_HAS_KHR_MAINTENANCE)
    khr_surface_maintenance_enabled_ =
      capabilities2_available &&
      extension_available(available,
                          VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
#endif
#if defined(AVB0795_DESKTOP_HAS_EXT_MAINTENANCE)
    ext_surface_maintenance_enabled_ =
      capabilities2_available &&
      extension_available(available,
                          VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
#endif
    if (!khr_surface_maintenance_enabled_ &&
        !ext_surface_maintenance_enabled_) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }
    append_unique(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
#if defined(AVB0795_DESKTOP_HAS_KHR_MAINTENANCE)
    if (khr_surface_maintenance_enabled_) {
      append_unique(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    }
#endif
#if defined(AVB0795_DESKTOP_HAS_EXT_MAINTENANCE)
    if (ext_surface_maintenance_enabled_) {
      append_unique(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    }
#endif

    const VkApplicationInfo application_info{
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pNext = nullptr,
      .pApplicationName = "avb0795",
      .applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
      .pEngineName = "avb0795 controller",
      .engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
      .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .pApplicationInfo = &application_info,
      .enabledLayerCount = 0,
      .ppEnabledLayerNames = nullptr,
      .enabledExtensionCount = static_cast<std::uint32_t>(enabled.size()),
      .ppEnabledExtensionNames = enabled.data(),
    };
    VkInstance instance{VK_NULL_HANDLE};
    result = vkCreateInstance(&create_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
      throw vulkan_error(render::vulkan_instance_create_error_code(result),
                         result);
    }
    instance_ = instance;
  }

  void DesktopHost::Impl::create_surface()
  {
    const auto result =
      glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }
  }

  void DesktopHost::Impl::select_physical_device()
  {
    std::uint32_t device_count{};
    auto result =
      vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (result != VK_SUCCESS || device_count == 0) {
      throw vulkan_error(
        core::AppErrorCode::UnsupportedGraphics,
        result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result);
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    result = vkEnumeratePhysicalDevices(instance_, &device_count,
                                        devices.data());
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::UnsupportedGraphics, result);
    }

    for (const auto candidate : devices) {
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(candidate, &properties);
      if (properties.apiVersion < VK_API_VERSION_1_3) {
        continue;
      }

      std::uint32_t extension_count{};
      result = vkEnumerateDeviceExtensionProperties(
        candidate, nullptr, &extension_count, nullptr);
      if (result != VK_SUCCESS) {
        continue;
      }
      std::vector<VkExtensionProperties> extensions(extension_count);
      result = vkEnumerateDeviceExtensionProperties(
        candidate, nullptr, &extension_count, extensions.data());
      if (result != VK_SUCCESS ||
          !extension_available(extensions,
                               VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        continue;
      }

      auto maintenance = MaintenanceVariant::None;
#if defined(AVB0795_DESKTOP_HAS_KHR_MAINTENANCE)
      if (khr_surface_maintenance_enabled_ &&
          extension_available(
            extensions, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR features{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
          .pNext = nullptr,
          .swapchainMaintenance1 = VK_FALSE,
        };
        VkPhysicalDeviceFeatures2 features2{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
          .pNext = &features,
        };
        vkGetPhysicalDeviceFeatures2(candidate, &features2);
        if (features.swapchainMaintenance1 == VK_TRUE) {
          maintenance = MaintenanceVariant::Khr;
        }
      }
#endif
#if defined(AVB0795_DESKTOP_HAS_EXT_MAINTENANCE)
      if (maintenance == MaintenanceVariant::None &&
          ext_surface_maintenance_enabled_ &&
          extension_available(
            extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT features{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
          .pNext = nullptr,
          .swapchainMaintenance1 = VK_FALSE,
        };
        VkPhysicalDeviceFeatures2 features2{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
          .pNext = &features,
        };
        vkGetPhysicalDeviceFeatures2(candidate, &features2);
        if (features.swapchainMaintenance1 == VK_TRUE) {
          maintenance = MaintenanceVariant::Ext;
        }
      }
#endif
      if (maintenance == MaintenanceVariant::None) {
        continue;
      }

      std::uint32_t queue_count{};
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count,
                                               nullptr);
      std::vector<VkQueueFamilyProperties> queues(queue_count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count,
                                               queues.data());
      std::optional<std::uint32_t> graphics_family;
      std::optional<std::uint32_t> present_family;
      for (std::uint32_t index{}; index < queue_count; ++index) {
        if (!graphics_family &&
            (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
          graphics_family = index;
        }
        VkBool32 supports_present{};
        if (!present_family &&
            vkGetPhysicalDeviceSurfaceSupportKHR(
              candidate, index, surface_, &supports_present) == VK_SUCCESS &&
            supports_present == VK_TRUE) {
          present_family = index;
        }
      }
      if (!graphics_family || !present_family) {
        continue;
      }

      std::uint32_t format_count{};
      std::uint32_t mode_count{};
      if (vkGetPhysicalDeviceSurfaceFormatsKHR(
            candidate, surface_, &format_count, nullptr) != VK_SUCCESS ||
          vkGetPhysicalDeviceSurfacePresentModesKHR(
            candidate, surface_, &mode_count, nullptr) != VK_SUCCESS ||
          format_count == 0 || mode_count == 0) {
        continue;
      }

      physical_device_ = candidate;
      maintenance_variant_ = maintenance;
      graphics_queue_family_ = *graphics_family;
      present_queue_family_ = *present_family;
      vkGetPhysicalDeviceMemoryProperties(candidate, &memory_properties_);
      return;
    }
    throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
  }

  void DesktopHost::Impl::create_device()
  {
    constexpr float priority{1.0F};
    std::array<VkDeviceQueueCreateInfo, 2> queue_infos{};
    queue_infos[0] = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .queueFamilyIndex = graphics_queue_family_,
      .queueCount = 1,
      .pQueuePriorities = &priority,
    };
    std::uint32_t queue_info_count{1};
    if (graphics_queue_family_ != present_queue_family_) {
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

#if defined(AVB0795_DESKTOP_HAS_KHR_MAINTENANCE)
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR khr_features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
      .pNext = nullptr,
      .swapchainMaintenance1 = VK_TRUE,
    };
#endif
#if defined(AVB0795_DESKTOP_HAS_EXT_MAINTENANCE)
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT ext_features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
      .pNext = nullptr,
      .swapchainMaintenance1 = VK_TRUE,
    };
#endif
    const void *enabled_features{};
    const char *maintenance_extension{};
    switch (maintenance_variant_) {
    case MaintenanceVariant::Khr:
#if defined(AVB0795_DESKTOP_HAS_KHR_MAINTENANCE)
      enabled_features = &khr_features;
      maintenance_extension =
        VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
#endif
      break;
    case MaintenanceVariant::Ext:
#if defined(AVB0795_DESKTOP_HAS_EXT_MAINTENANCE)
      enabled_features = &ext_features;
      maintenance_extension =
        VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
#endif
      break;
    case MaintenanceVariant::None: break;
    }
    if (enabled_features == nullptr || maintenance_extension == nullptr) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }
    const std::array extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                maintenance_extension};
    const VkDeviceCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = enabled_features,
      .flags = 0,
      .queueCreateInfoCount = queue_info_count,
      .pQueueCreateInfos = queue_infos.data(),
      .enabledLayerCount = 0,
      .ppEnabledLayerNames = nullptr,
      .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
      .ppEnabledExtensionNames = extensions.data(),
      .pEnabledFeatures = nullptr,
    };
    VkDevice device{VK_NULL_HANDLE};
    const auto result =
      vkCreateDevice(physical_device_, &create_info, nullptr, &device);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::UnsupportedGraphics, result);
    }
    device_ = device;
    vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, present_queue_family_, 0, &present_queue_);

    bool release_loaded{};
    switch (maintenance_variant_) {
    case MaintenanceVariant::Khr:
#if defined(AVB0795_DESKTOP_HAS_KHR_MAINTENANCE)
      release_images_khr_ =
        reinterpret_cast<PFN_vkReleaseSwapchainImagesKHR>(
          vkGetDeviceProcAddr(device_, "vkReleaseSwapchainImagesKHR"));
      release_loaded = release_images_khr_ != nullptr;
#endif
      break;
    case MaintenanceVariant::Ext:
#if defined(AVB0795_DESKTOP_HAS_EXT_MAINTENANCE)
      release_images_ext_ =
        reinterpret_cast<PFN_vkReleaseSwapchainImagesEXT>(
          vkGetDeviceProcAddr(device_, "vkReleaseSwapchainImagesEXT"));
      release_loaded = release_images_ext_ != nullptr;
#endif
      break;
    case MaintenanceVariant::None: break;
    }
    if (!release_loaded) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }
  }

  void DesktopHost::Impl::create_common_resources()
  {
    const VkCommandPoolCreateInfo command_pool_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .pNext = nullptr,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = graphics_queue_family_,
    };
    VkCommandPool command_pool{VK_NULL_HANDLE};
    auto result = vkCreateCommandPool(device_, &command_pool_info, nullptr,
                                      &command_pool);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }
    command_pool_ = command_pool;

    constexpr std::array pool_sizes{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 32},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 32},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 32},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 32},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 32},
    };
    const VkDescriptorPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .pNext = nullptr,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = 128,
      .poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size()),
      .pPoolSizes = pool_sizes.data(),
    };
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    result = vkCreateDescriptorPool(device_, &pool_info, nullptr,
                                    &descriptor_pool);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }
    descriptor_pool_ = descriptor_pool;
  }

  std::expected<void, core::AppError> DesktopHost::Impl::create_swapchain(
    const VkExtent2D requested, const bool recovery_candidate)
  {
    if (zero_extent(requested)) {
      return {};
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    auto result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      physical_device_, surface_, &capabilities);
    if (result != VK_SUCCESS) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure, result)};
    }

    std::uint32_t format_count{};
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
      physical_device_, surface_, &format_count, nullptr);
    if (result != VK_SUCCESS || format_count == 0) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure,
        result == VK_SUCCESS ? VK_ERROR_FORMAT_NOT_SUPPORTED : result)};
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
      physical_device_, surface_, &format_count, formats.data());
    if (result != VK_SUCCESS) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    const auto preferred = std::ranges::find_if(
      formats, [](const VkSurfaceFormatKHR &format) noexcept {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      });
    const auto selected_format =
      preferred != formats.end() ? *preferred : formats.front();
    if (imgui_vulkan_initialized_ && selected_format.format != imgui_format_) {
      return std::unexpected{
        graphics_error(core::AppErrorCode::UnsupportedGraphics)};
    }

    std::uint32_t mode_count{};
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
      physical_device_, surface_, &mode_count, nullptr);
    if (result != VK_SUCCESS || mode_count == 0) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure,
        result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result)};
    }
    std::vector<VkPresentModeKHR> modes(mode_count);
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
      physical_device_, surface_, &mode_count, modes.data());
    if (result != VK_SUCCESS ||
        std::ranges::find(modes, VK_PRESENT_MODE_FIFO_KHR) == modes.end()) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure,
        result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result)};
    }

    VkExtent2D extent{};
    if (capabilities.currentExtent.width !=
        (std::numeric_limits<std::uint32_t>::max)()) {
      extent = capabilities.currentExtent;
    }
    else {
      extent = {
        .width = std::clamp(requested.width,
                            capabilities.minImageExtent.width,
                            capabilities.maxImageExtent.width),
        .height = std::clamp(requested.height,
                             capabilities.minImageExtent.height,
                             capabilities.maxImageExtent.height),
      };
    }
    if (zero_extent(extent)) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure,
        VK_ERROR_OUT_OF_DATE_KHR)};
    }

    std::uint32_t image_count = (std::max) (capabilities.minImageCount, 2U);
    if (image_count < (std::numeric_limits<std::uint32_t>::max)()) {
      ++image_count;
    }
    if (capabilities.maxImageCount != 0) {
      image_count = (std::min) (image_count, capabilities.maxImageCount);
    }
    if (image_count < 2) {
      return std::unexpected{
        graphics_error(core::AppErrorCode::UnsupportedGraphics)};
    }

    constexpr std::array composite_candidates{
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    const auto composite = std::ranges::find_if(
      composite_candidates,
      [&capabilities](const VkCompositeAlphaFlagBitsKHR value) noexcept {
        return (capabilities.supportedCompositeAlpha & value) != 0;
      });
    if (composite == composite_candidates.end()) {
      return std::unexpected{
        graphics_error(core::AppErrorCode::UnsupportedGraphics)};
    }

    const auto active_ownership =
      active_swapchain_ ? ownership(active_swapchain_->handle)
                        : render::SwapchainOwnershipFacts{};
    const auto creation = render::plan_same_surface_swapchain_create(
      active_swapchain_.has_value(), active_ownership);
    if (!creation.may_create) {
      return std::unexpected{
        graphics_error(core::AppErrorCode::RuntimeStateLost)};
    }

    const auto old_swapchain =
      creation.use_active_as_old ? active_swapchain_->handle : VK_NULL_HANDLE;
    const std::array queue_families{graphics_queue_family_,
                                    present_queue_family_};
    const bool separate_queues =
      graphics_queue_family_ != present_queue_family_;
    const VkSwapchainCreateInfoKHR create_info{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .pNext = nullptr,
      .flags = 0,
      .surface = surface_,
      .minImageCount = image_count,
      .imageFormat = selected_format.format,
      .imageColorSpace = selected_format.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode =
        separate_queues ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = separate_queues ? 2U : 0U,
      .pQueueFamilyIndices =
        separate_queues ? queue_families.data() : nullptr,
      .preTransform = capabilities.currentTransform,
      .compositeAlpha = *composite,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
      .oldSwapchain = old_swapchain,
    };

    if (creation.retire_active_before_create) {
      // Passing an old swapchain to replacement creation cannot be rolled back
      // into new acquisitions on that old owner if creation fails. Retire first;
      // outstanding presentations keep it alive while later retries start afresh.
      retire_active_swapchain();
      made_progress_ = true;
    }

    SwapchainState candidate{
      .format = selected_format.format,
      .extent = extent,
      .minimum_image_count = (std::max) (capabilities.minImageCount, 2U),
      .recovery_candidate = recovery_candidate,
    };
    result =
      vkCreateSwapchainKHR(device_, &create_info, nullptr, &candidate.handle);
    if (result != VK_SUCCESS) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    try {
      if (auto resources = create_swapchain_resources(candidate); !resources) {
        destroy_swapchain_state(candidate);
        return resources;
      }
    } catch (...) {
      destroy_swapchain_state(candidate);
      throw;
    }
    active_swapchain_.emplace(std::move(candidate));
    return {};
  }

  std::expected<void, core::AppError>
  DesktopHost::Impl::create_swapchain_resources(SwapchainState &state)
  {
    std::uint32_t image_count{};
    auto result =
      vkGetSwapchainImagesKHR(device_, state.handle, &image_count, nullptr);
    if (result != VK_SUCCESS || image_count < 2) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure,
        result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result)};
    }
    state.images.resize(image_count);
    result = vkGetSwapchainImagesKHR(device_, state.handle, &image_count,
                                     state.images.data());
    if (result != VK_SUCCESS) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure, result)};
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
      VkImageView view{VK_NULL_HANDLE};
      result = vkCreateImageView(device_, &view_info, nullptr, &view);
      if (result != VK_SUCCESS) {
        return std::unexpected{vulkan_error(
          core::AppErrorCode::RuntimeConstructionFailure, result)};
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
    VkRenderPass render_pass{VK_NULL_HANDLE};
    result = vkCreateRenderPass(device_, &render_pass_info, nullptr,
                                &render_pass);
    if (result != VK_SUCCESS) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure, result)};
    }
    state.render_pass = render_pass;

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
      VkFramebuffer framebuffer{VK_NULL_HANDLE};
      result = vkCreateFramebuffer(device_, &framebuffer_info, nullptr,
                                   &framebuffer);
      if (result != VK_SUCCESS) {
        return std::unexpected{vulkan_error(
          core::AppErrorCode::RuntimeConstructionFailure, result)};
      }
      state.framebuffers.push_back(framebuffer);
    }
    return {};
  }

  void DesktopHost::Impl::initialize_imgui()
  {
    if (!active_swapchain_) {
      throw graphics_error(core::AppErrorCode::RuntimeConstructionFailure);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imgui_context_created_ = true;
    // Application owns persisted preferences; controller geometry is session-only.
    // Disable both the implicit CWD/imgui.ini read and automatic writes.
    ImGui::GetIO().IniFilename = nullptr;
    if (const auto configured = configure_controller_font(*ImGui::GetIO().Fonts, font_data_); !configured) {
      throw configured.error();
    }
    ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    if (!ImGui_ImplGlfw_InitForVulkan(window_, true)) {
      throw graphics_error(core::AppErrorCode::RuntimeConstructionFailure);
    }
    imgui_glfw_initialized_ = true;

    backend_frame_capacity_ = active_swapchain_->images.size();
    imgui_format_ = active_swapchain_->format;
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = instance_;
    init_info.PhysicalDevice = physical_device_;
    init_info.Device = device_;
    init_info.QueueFamily = graphics_queue_family_;
    init_info.Queue = graphics_queue_;
    init_info.DescriptorPool = descriptor_pool_;
    init_info.MinImageCount = active_swapchain_->minimum_image_count;
    init_info.ImageCount =
      static_cast<std::uint32_t>(backend_frame_capacity_);
    init_info.PipelineInfoMain.RenderPass = active_swapchain_->render_pass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.UseDynamicRendering = false;
    init_info.CheckVkResultFn = check_backend_result;

    try {
      if (!ImGui_ImplVulkan_Init(&init_info)) {
        throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                           VK_ERROR_INITIALIZATION_FAILED);
      }
      imgui_vulkan_initialized_ = true;
    } catch (...) {
      // ImGui attaches its value-initialized renderer data before creating any
      // Vulkan child objects. Let Shutdown release a partially created backend
      // before the host destroys its device.
      imgui_vulkan_initialized_ =
        ImGui::GetIO().BackendRendererUserData != nullptr;
      throw;
    }

    // The stock 1.92 backend uploads dynamic textures with vkQueueWaitIdle.
    // avb0795 keeps texture work on its classified, fence-owned submission
    // path instead, so the backend must never process ImDrawData::Textures.
    ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
  }

  std::optional<std::uint32_t> DesktopHost::Impl::memory_type(
    std::uint32_t bits, const VkMemoryPropertyFlags required, const VkMemoryPropertyFlags preferred) const noexcept
  {
    std::optional<std::uint32_t> required_only;
    for (std::uint32_t index{}; index < memory_properties_.memoryTypeCount;
         ++index) {
      if ((bits & (std::uint32_t{1} << index)) == 0) {
        continue;
      }
      const auto flags = memory_properties_.memoryTypes[index].propertyFlags;
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

  void DesktopHost::Impl::create_font_texture()
  {
    unsigned char *pixels{};
    int width{};
    int height{};
    int bytes_per_pixel{};
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(
      &pixels, &width, &height, &bytes_per_pixel);
    if (pixels == nullptr || width <= 0 || height <= 0 ||
        bytes_per_pixel != 4) {
      throw graphics_error(core::AppErrorCode::RuntimeConstructionFailure);
    }
    const auto width_size = static_cast<std::uint64_t>(width);
    const auto height_size = static_cast<std::uint64_t>(height);
    constexpr std::uint64_t pixel_size{4};
    const auto maximum =
      static_cast<std::uint64_t>((std::numeric_limits<VkDeviceSize>::max)());
    if (width_size > maximum / height_size ||
        width_size * height_size > maximum / pixel_size) {
      throw graphics_error(core::AppErrorCode::RuntimeConstructionFailure);
    }
    const auto upload_size = static_cast<VkDeviceSize>(
      width_size * height_size * pixel_size);
    font_texture_.width = static_cast<std::uint32_t>(width);
    font_texture_.height = static_cast<std::uint32_t>(height);

    const VkImageCreateInfo image_info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent =
        {
          .width = font_texture_.width,
          .height = font_texture_.height,
          .depth = 1,
        },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = 0,
      .pQueueFamilyIndices = nullptr,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image{VK_NULL_HANDLE};
    auto result = vkCreateImage(device_, &image_info, nullptr, &image);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }
    font_texture_.image = image;
    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(device_, font_texture_.image,
                                 &image_requirements);
    const auto image_memory =
      memory_type(image_requirements.memoryTypeBits,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!image_memory) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }
    const VkMemoryAllocateInfo image_allocation{
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = nullptr,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = *image_memory,
    };
    VkDeviceMemory image_memory_handle{VK_NULL_HANDLE};
    result = vkAllocateMemory(device_, &image_allocation, nullptr,
                              &image_memory_handle);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }
    font_texture_.memory = image_memory_handle;
    result = vkBindImageMemory(device_, font_texture_.image,
                               font_texture_.memory, 0);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }

    const VkImageViewCreateInfo view_info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .image = font_texture_.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
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
    VkImageView image_view{VK_NULL_HANDLE};
    result = vkCreateImageView(device_, &view_info, nullptr, &image_view);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }
    font_texture_.view = image_view;

    const VkBufferCreateInfo buffer_info{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .size = upload_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = 0,
      .pQueueFamilyIndices = nullptr,
    };
    VkBuffer staging_buffer{VK_NULL_HANDLE};
    result =
      vkCreateBuffer(device_, &buffer_info, nullptr, &staging_buffer);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }
    font_texture_.staging_buffer = staging_buffer;
    VkMemoryRequirements buffer_requirements{};
    vkGetBufferMemoryRequirements(device_, font_texture_.staging_buffer,
                                  &buffer_requirements);
    const auto buffer_memory =
      memory_type(buffer_requirements.memoryTypeBits,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!buffer_memory) {
      throw graphics_error(core::AppErrorCode::UnsupportedGraphics);
    }
    const VkMemoryAllocateInfo buffer_allocation{
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = nullptr,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex = *buffer_memory,
    };
    VkDeviceMemory staging_memory{VK_NULL_HANDLE};
    result = vkAllocateMemory(device_, &buffer_allocation, nullptr,
                              &staging_memory);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }
    font_texture_.staging_memory = staging_memory;
    result = vkBindBufferMemory(device_, font_texture_.staging_buffer,
                                font_texture_.staging_memory, 0);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }
    void *mapped{};
    result = vkMapMemory(device_, font_texture_.staging_memory, 0,
                         upload_size, 0, &mapped);
    if (result != VK_SUCCESS || mapped == nullptr) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result == VK_SUCCESS ? VK_ERROR_MEMORY_MAP_FAILED
                                              : result);
    }
    std::memcpy(mapped, pixels, static_cast<std::size_t>(upload_size));
    const auto memory_flags =
      memory_properties_.memoryTypes[*buffer_memory].propertyFlags;
    if ((memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
      const VkMappedMemoryRange range{
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .pNext = nullptr,
        .memory = font_texture_.staging_memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
      };
      result = vkFlushMappedMemoryRanges(device_, 1, &range);
    }
    vkUnmapMemory(device_, font_texture_.staging_memory);
    if (result != VK_SUCCESS) {
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         result);
    }

    font_texture_.descriptor = ImGui_ImplVulkan_AddTexture(
      font_texture_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (font_texture_.descriptor == VK_NULL_HANDLE) {
      font_texture_.descriptor = VK_NULL_HANDLE;
      throw vulkan_error(core::AppErrorCode::RuntimeConstructionFailure,
                         VK_ERROR_INITIALIZATION_FAILED);
    }
    auto *const texture_data = ImGui::GetIO().Fonts->TexData;
    if (texture_data == nullptr) {
      throw graphics_error(core::AppErrorCode::RuntimeConstructionFailure);
    }
    texture_data->SetTexID(
      reinterpret_cast<ImTextureID>(font_texture_.descriptor));
    texture_data->SetStatus(ImTextureStatus_OK);
    texture_data->BackendUserData = nullptr;
    font_texture_.upload_pending = true;
  }

  void DesktopHost::Impl::create_frame_resources()
  {
    // Resize before native creation: vector allocation cannot orphan a returned
    // plain handle record, and every successfully built prefix stays owned.
    frames_.resize(backend_frame_capacity_);
    presents_.resize(backend_frame_capacity_);
    for (auto &record : frames_) {
      auto created = create_frame_record();
      if (!created) throw created.error();
      record = *created;
    }
    for (auto &record : presents_) {
      auto created = create_present_record();
      if (!created) throw created.error();
      record = *created;
    }
  }

  std::expected<DesktopHost::Impl::FrameRecord, core::AppError>
  DesktopHost::Impl::create_frame_record()
  {
    FrameRecord record;
    const VkCommandBufferAllocateInfo allocation{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .pNext = nullptr,
      .commandPool = command_pool_,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer{VK_NULL_HANDLE};
    auto result =
      vkAllocateCommandBuffers(device_, &allocation, &command_buffer);
    if (result != VK_SUCCESS) {
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeStateLost, result)};
    }
    record.command_buffer = command_buffer;
    observe_performance(PerformanceCounter::CommandBuffersCreated);
    const VkFenceCreateInfo fence_info{
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
    };
    VkFence acquire_fence{VK_NULL_HANDLE};
    result = vkCreateFence(device_, &fence_info, nullptr, &acquire_fence);
    if (result == VK_SUCCESS) {
      record.acquire_fence = acquire_fence;
      observe_performance(PerformanceCounter::FencesCreated);
      VkFence submission_fence{VK_NULL_HANDLE};
      result =
        vkCreateFence(device_, &fence_info, nullptr, &submission_fence);
      if (result == VK_SUCCESS) {
        record.submission_fence = submission_fence;
        observe_performance(PerformanceCounter::FencesCreated);
      }
    }
    const VkSemaphoreCreateInfo semaphore_info{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
    };
    if (result == VK_SUCCESS) {
      VkSemaphore acquire_semaphore{VK_NULL_HANDLE};
      result = vkCreateSemaphore(device_, &semaphore_info, nullptr,
                                 &acquire_semaphore);
      if (result == VK_SUCCESS) {
        record.acquire_semaphore = acquire_semaphore;
        observe_performance(PerformanceCounter::SemaphoresCreated);
      }
    }
    if (result != VK_SUCCESS) {
      destroy_frame_record(record);
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeStateLost, result)};
    }
    return record;
  }

  std::expected<DesktopHost::Impl::PresentRecord, core::AppError>
  DesktopHost::Impl::create_present_record()
  {
    PresentRecord record;
    const VkFenceCreateInfo fence_info{
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VkFence fence{VK_NULL_HANDLE};
    auto result = vkCreateFence(device_, &fence_info, nullptr, &fence);
    if (result == VK_SUCCESS) {
      record.fence = fence;
      observe_performance(PerformanceCounter::FencesCreated);
      const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      };
      VkSemaphore semaphore{VK_NULL_HANDLE};
      result = vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore);
      if (result == VK_SUCCESS) {
        record.wait_semaphore = semaphore;
        observe_performance(PerformanceCounter::SemaphoresCreated);
      }
    }
    if (result != VK_SUCCESS) {
      destroy_present_record(record);
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeStateLost, result)};
    }
    return record;
  }

  bool DesktopHost::Impl::recycle_present(
    const render::PresentUseKey key, const render::PresentUsePhase phase) noexcept
  {
    if (key.index >= presents_.size() ||
        !render::release_present_use(presents_[key.index].use, key, key.index, phase)) {
      enter_fatal(FatalKind::Unexpected,
                  graphics_error(core::AppErrorCode::RuntimeStateLost));
      return false;
    }
    auto &record = presents_[key.index];
    record.swapchain = VK_NULL_HANDLE;
    record.confirms_recovery = false;
    record.counts_as_presented = false;
    // Consume the use before any fallible reset. Its fence must never be polled
    // or accounted a second time if reset fails and closes admission.
    auto result = phase == render::PresentUsePhase::PresentPending
                    ? vkResetFences(device_, 1, &record.fence) : VK_SUCCESS;
    if (result == VK_SUCCESS && record.wait_semaphore == VK_NULL_HANDLE) {
      const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      };
      VkSemaphore semaphore{VK_NULL_HANDLE};
      result = vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore);
      if (result == VK_SUCCESS) {
        record.wait_semaphore = semaphore;
        observe_performance(PerformanceCounter::SemaphoresCreated);
      }
    }
    if (result != VK_SUCCESS) {
      enter_fatal(result == VK_ERROR_DEVICE_LOST ? FatalKind::DeviceLost
                                                : FatalKind::Terminal,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost, result));
      return false;
    }
    return true;
  }

  void DesktopHost::Impl::recycle_frame(FrameRecord &record) noexcept
  {
    const auto prior_phase = record.phase;
    const auto present_use = record.present_use;
    record.phase = FramePhase::Free;
    record.present_use.reset();
    record.swapchain = VK_NULL_HANDLE;
    record.image_index = 0;
    record.acquired = false;
    record.uploads_font = false;
    made_progress_ = true;
    if (present_use &&
        !recycle_present(*present_use, render::PresentUsePhase::Reserved)) {
      return;
    }
    if (prior_phase == FramePhase::Ready) {
      // An acquire without an image leaves all supplied synchronization objects
      // unchanged, and no command recording took place for this reservation.
      return;
    }
    auto result = vkResetCommandBuffer(record.command_buffer, 0);
    if (result == VK_SUCCESS) {
      const std::array fences{record.acquire_fence, record.submission_fence};
      result = vkResetFences(device_, static_cast<std::uint32_t>(fences.size()),
                             fences.data());
    }
    if (result == VK_SUCCESS && record.acquire_semaphore == VK_NULL_HANDLE) {
      const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      };
      VkSemaphore semaphore{VK_NULL_HANDLE};
      result = vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore);
      if (result == VK_SUCCESS) {
        record.acquire_semaphore = semaphore;
        observe_performance(PerformanceCounter::SemaphoresCreated);
      }
    }
    if (result != VK_SUCCESS) {
      enter_fatal(result == VK_ERROR_DEVICE_LOST ? FatalKind::DeviceLost
                                                : FatalKind::Terminal,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost, result));
    }
  }

  void DesktopHost::Impl::discard_unconsumed_signals(FrameRecord &record) noexcept
  {
    if (record.phase == FramePhase::AcquireProofPending &&
        record.acquire_semaphore != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, record.acquire_semaphore, nullptr);
      record.acquire_semaphore = VK_NULL_HANDLE;
      observe_performance(PerformanceCounter::SemaphoresDestroyed);
    }
    if (record.phase != FramePhase::PresentNotEnqueued &&
        record.phase != FramePhase::SubmitUncertain) {
      return;
    }
    if (!record.present_use || record.present_use->index >= presents_.size() ||
        !render::matches_present_use(presents_[record.present_use->index].use,
                                     *record.present_use, record.present_use->index,
                                     render::PresentUsePhase::Reserved)) {
      enter_fatal(FatalKind::Unexpected,
                  graphics_error(core::AppErrorCode::RuntimeStateLost));
      return;
    }
    auto &present = presents_[record.present_use->index];
    if (present.wait_semaphore != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, present.wait_semaphore, nullptr);
      present.wait_semaphore = VK_NULL_HANDLE;
      observe_performance(PerformanceCounter::SemaphoresDestroyed);
    }
  }

  VkResult DesktopHost::Impl::record_frame(FrameRecord &record,
                                           ImDrawData &draw_data)
  {
    if (!active_swapchain_ ||
        record.image_index >= active_swapchain_->framebuffers.size()) {
      return VK_ERROR_OUT_OF_DATE_KHR;
    }
    const VkCommandBufferBeginInfo begin_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .pNext = nullptr,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      .pInheritanceInfo = nullptr,
    };
    auto result = vkBeginCommandBuffer(record.command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
      return result;
    }

    if (font_texture_.upload_pending && !font_texture_.upload_in_flight) {
      const VkBufferMemoryBarrier buffer_barrier{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = font_texture_.staging_buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
      };
      const VkImageMemoryBarrier copy_barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = font_texture_.image,
        .subresourceRange =
          {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
          },
      };
      vkCmdPipelineBarrier(
        record.command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &buffer_barrier, 1,
        &copy_barrier);
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
            .width = font_texture_.width,
            .height = font_texture_.height,
            .depth = 1,
          },
      };
      vkCmdCopyBufferToImage(record.command_buffer,
                             font_texture_.staging_buffer,
                             font_texture_.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      const VkImageMemoryBarrier use_barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = font_texture_.image,
        .subresourceRange =
          {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
          },
      };
      vkCmdPipelineBarrier(
        record.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &use_barrier);
      record.uploads_font = true;
    }

    const VkClearValue clear{.color = {{0.08F, 0.09F, 0.11F, 1.0F}}};
    const VkRenderPassBeginInfo render_pass{
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .pNext = nullptr,
      .renderPass = active_swapchain_->render_pass,
      .framebuffer =
        active_swapchain_->framebuffers[record.image_index],
      .renderArea = {.offset = {}, .extent = active_swapchain_->extent},
      .clearValueCount = 1,
      .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(record.command_buffer, &render_pass,
                         VK_SUBPASS_CONTENTS_INLINE);
    draw_data.Textures = nullptr;
    ImGui_ImplVulkan_RenderDrawData(&draw_data, record.command_buffer);
    vkCmdEndRenderPass(record.command_buffer);
    return vkEndCommandBuffer(record.command_buffer);
  }

  void DesktopHost::Impl::attempt_present(
    ImDrawData &draw_data, const std::chrono::steady_clock::time_point now)
  {
    if (stopping_controller_ || swapchain_recreate_pending_ ||
        !active_swapchain_ || zero_extent(active_swapchain_->extent)) {
      return;
    }
    // ImGui rotates by RenderDrawData call count, independently of record index.
    // Each successful recording is submitted to this one graphics queue. A later
    // submission fence also proves earlier draw-buffer reads complete, so at most
    // C active frames protects its C buffers even when records retire out of order.
    // Acquire-only and release-retained uses make the admission bound stricter.
    const auto frame = std::ranges::find_if(frames_, [](const auto &record) {
      return record.phase == FramePhase::Free;
    });
    const auto available_present = std::ranges::find_if(presents_, [](const auto &record) {
      return record.use.phase == render::PresentUsePhase::Free;
    });
    if (frame == frames_.end() || available_present == presents_.end()) {
      schedule_poll(now, false);
      return;
    }
    const auto present_index = static_cast<std::size_t>(available_present - presents_.begin());
    const auto present_use = render::reserve_present_use(
      available_present->use, present_index, last_present_use_serial_);
    if (!present_use) {
      enter_fatal(FatalKind::Terminal,
                  graphics_error(core::AppErrorCode::RuntimeStateLost));
      return;
    }

    auto &record = *frame;
    record.phase = FramePhase::Ready;
    record.present_use = *present_use;
    auto &present = *available_present;
    std::uint32_t image_index{
      (std::numeric_limits<std::uint32_t>::max)()};
    const auto acquire_result = vkAcquireNextImageKHR(
      device_, active_swapchain_->handle, 0,
      record.acquire_semaphore, record.acquire_fence,
      &image_index);
    const auto acquire =
      render::classify_acquire_result(acquire_result, false);
    if (acquire.ownership == render::AcquireOwnership::Unchanged) {
      recycle_frame(record);
    }
    switch (acquire.action) {
    case render::AcquireAction::Wait:
      schedule_poll(now, false);
      return;
    case render::AcquireAction::RecreateSwapchain:
      swapchain_recreate_pending_ = true;
      schedule_poll(now, false);
      return;
    case render::AcquireAction::RecreateSurface:
      enter_fatal(FatalKind::SurfaceLost,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost,
                               acquire_result));
      return;
    case render::AcquireAction::RebuildDevice:
      enter_fatal(FatalKind::DeviceLost,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost,
                               acquire_result));
      return;
    case render::AcquireAction::Terminal:
      enter_fatal(FatalKind::Terminal,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost,
                               acquire_result));
      return;
    case render::AcquireAction::UseImage: break;
    }

    record.swapchain = active_swapchain_->handle;
    record.image_index = image_index;
    record.acquired = true;
    record.phase = FramePhase::AcquireProofPending;
    const auto command_result = record_frame(record, draw_data);
    if (command_result != VK_SUCCESS) {
      if (command_result == VK_ERROR_DEVICE_LOST) {
        enter_fatal(FatalKind::DeviceLost,
                    vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                 command_result));
      }
      else {
        enter_fatal(FatalKind::Terminal,
                    vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                 command_result));
      }
      schedule_poll(now, false);
      return;
    }

    // Publish the acquired image's ownership record before the native submit.
    // Once submission is attempted, an error may mean uncertain execution rather
    // than no work; keeping the record lets the drain path demand the right proof.
    constexpr VkPipelineStageFlags wait_stage{
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    const VkSubmitInfo submit_info{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .pNext = nullptr,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &record.acquire_semaphore,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1,
      .pCommandBuffers = &record.command_buffer,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &present.wait_semaphore,
    };
    const auto submit_result = vkQueueSubmit(
      graphics_queue_, 1, &submit_info, record.submission_fence);
    const auto submit = render::classify_submit_result(submit_result);
    switch (submit) {
    case render::SubmitOutcome::NotEnqueued:
      record.phase = FramePhase::AcquireProofPending;
      enter_fatal(FatalKind::Terminal,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost,
                               submit_result));
      schedule_poll(now, false);
      return;
    case render::SubmitOutcome::DeviceLost:
      record.phase = FramePhase::SubmitUncertain;
      enter_fatal(FatalKind::DeviceLost,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost,
                               submit_result));
      return;
    case render::SubmitOutcome::Uncertain:
      record.phase = FramePhase::SubmitUncertain;
      if (record.uploads_font) {
        font_texture_.upload_pending = false;
        font_texture_.upload_in_flight = true;
      }
      enter_fatal(FatalKind::Terminal,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost,
                               submit_result));
      schedule_poll(now, false);
      return;
    case render::SubmitOutcome::Enqueued:
      observe_performance(PerformanceCounter::Submitted);
      record.phase = FramePhase::Submitted;
      if (record.uploads_font) {
        font_texture_.upload_pending = false;
        font_texture_.upload_in_flight = true;
      }
      break;
    }

    present.swapchain = record.swapchain;
    present.confirms_recovery = active_swapchain_->recovery_candidate;

#if defined(AVB0795_DESKTOP_HAS_KHR_MAINTENANCE)
    const VkSwapchainPresentFenceInfoKHR present_fence_khr{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
      .pNext = nullptr,
      .swapchainCount = 1,
      .pFences = &present.fence,
    };
#endif
#if defined(AVB0795_DESKTOP_HAS_EXT_MAINTENANCE)
    const VkSwapchainPresentFenceInfoEXT present_fence_ext{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
      .pNext = nullptr,
      .swapchainCount = 1,
      .pFences = &present.fence,
    };
#endif
    const void *present_fence_info{};
    switch (maintenance_variant_) {
    case MaintenanceVariant::Khr:
#if defined(AVB0795_DESKTOP_HAS_KHR_MAINTENANCE)
      present_fence_info = &present_fence_khr;
#endif
      break;
    case MaintenanceVariant::Ext:
#if defined(AVB0795_DESKTOP_HAS_EXT_MAINTENANCE)
      present_fence_info = &present_fence_ext;
#endif
      break;
    case MaintenanceVariant::None: break;
    }
    if (present_fence_info == nullptr) {
      record.phase = FramePhase::PresentNotEnqueued;
      enter_fatal(FatalKind::Terminal,
                  graphics_error(core::AppErrorCode::UnsupportedGraphics));
      schedule_poll(now, false);
      return;
    }
    const VkPresentInfoKHR present_info{
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .pNext = present_fence_info,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &present.wait_semaphore,
      .swapchainCount = 1,
      .pSwapchains = &record.swapchain,
      .pImageIndices = &record.image_index,
      .pResults = nullptr,
    };
    const auto present_result = vkQueuePresentKHR(present_queue_, &present_info);
    const auto classification =
      render::classify_present_result(present_result);
    if (classification.ownership == render::PresentOwnership::NotEnqueued) {
      record.phase = FramePhase::PresentNotEnqueued;
      enter_fatal(FatalKind::Terminal,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost,
                               present_result));
      schedule_poll(now, false);
      return;
    }

    // A queue fence covers draw commands; this semaphore is now owned by the
    // independent present operation and cannot retire with that queue fence.
    if (!render::enqueue_present_use(present.use, *present_use, present_index)) {
      enter_fatal(FatalKind::Unexpected,
                  graphics_error(core::AppErrorCode::RuntimeStateLost));
      return;
    }
    present.counts_as_presented = present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR;
    record.present_use.reset();
    record.acquired = false;
    record.phase = FramePhase::Submitted;
    schedule_poll(now, false);

    switch (classification.recovery) {
    case render::PresentRecovery::None: break;
    case render::PresentRecovery::RecreateSwapchain:
      swapchain_recreate_pending_ = true;
      break;
    case render::PresentRecovery::RecreateSurface:
      enter_fatal(FatalKind::SurfaceLost,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost,
                               present_result));
      break;
    case render::PresentRecovery::RebuildDevice:
      enter_fatal(FatalKind::DeviceLost,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost,
                               present_result));
      break;
    case render::PresentRecovery::Terminal:
      enter_fatal(FatalKind::Terminal,
                  vulkan_error(core::AppErrorCode::RuntimeStateLost,
                               present_result));
      break;
    }
    if (acquire.recreate_after_safe_use) {
      swapchain_recreate_pending_ = true;
    }
  }

  VkResult DesktopHost::Impl::release_image(
    const VkSwapchainKHR swapchain,
    const std::uint32_t image_index) noexcept
  {
    switch (maintenance_variant_) {
    case MaintenanceVariant::Khr:
#if defined(AVB0795_DESKTOP_HAS_KHR_MAINTENANCE)
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
#if defined(AVB0795_DESKTOP_HAS_EXT_MAINTENANCE)
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
    case MaintenanceVariant::None: break;
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }

  void DesktopHost::Impl::progress_frames(
    const std::chrono::steady_clock::time_point now, const bool draining)
  {
    for (auto &record : frames_) {
      if (record.phase == FramePhase::Free ||
          record.phase == FramePhase::ReleaseRetained) {
        continue;
      }

      VkFence proof{VK_NULL_HANDLE};
      switch (record.phase) {
      case FramePhase::AcquireProofPending:
        proof = record.acquire_fence;
        break;
      case FramePhase::Submitted:
      case FramePhase::PresentNotEnqueued:
      case FramePhase::SubmitUncertain:
        proof = record.submission_fence;
        break;
      case FramePhase::Free:
      case FramePhase::Ready:
      case FramePhase::ReleaseRetained: break;
      }
      if (proof == VK_NULL_HANDLE) {
        enter_fatal(FatalKind::Terminal,
                    graphics_error(core::AppErrorCode::RuntimeStateLost));
        continue;
      }

      auto status = vkGetFenceStatus(device_, proof);
      if (status == VK_SUCCESS && proof != record.acquire_fence) {
        // The acquire fence signal follows its semaphore signal independently of
        // the rendering submission. Observe it before resetting frame owners.
        status = vkGetFenceStatus(device_, record.acquire_fence);
      }
      if (status == VK_NOT_READY) {
        schedule_poll(now, draining);
        continue;
      }
      if (status == VK_ERROR_DEVICE_LOST) {
        enter_fatal(FatalKind::DeviceLost,
                    vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                 status));
        return;
      }
      if (status != VK_SUCCESS) {
        enter_fatal(FatalKind::Terminal,
                    vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                 status));
        schedule_poll(now, draining);
        continue;
      }

      const bool submission_completed =
        record.phase != FramePhase::AcquireProofPending;
      if (submission_completed && record.uploads_font &&
          font_texture_.upload_in_flight) {
        font_texture_.upload_in_flight = false;
        destroy_font_staging();
      }
      if (!record.acquired) {
        recycle_frame(record);
        if (fatal_kind_ == FatalKind::DeviceLost || fatal_kind_ == FatalKind::Unexpected) return;
        continue;
      }

      const auto release_result =
        release_image(record.swapchain, record.image_index);
      const auto release =
        render::classify_release_result(release_result);
      if (release_result == VK_ERROR_DEVICE_LOST) {
        record.phase = FramePhase::ReleaseRetained;
        enter_fatal(FatalKind::DeviceLost,
                    vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                 release_result));
        return;
      }
      discard_unconsumed_signals(record);
      if (fatal_kind_ == FatalKind::Unexpected) return;
      if (release.ownership == render::ReleaseOwnership::Released) {
        recycle_frame(record);
        if (fatal_kind_ == FatalKind::DeviceLost || fatal_kind_ == FatalKind::Unexpected) return;
        continue;
      }

      record.phase = FramePhase::ReleaseRetained;
      if (release.recovery == render::ReleaseRecovery::SurfaceLost) {
        enter_fatal(FatalKind::SurfaceLost,
                    vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                 release_result));
      }
      else {
        enter_fatal(FatalKind::Terminal,
                    vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                 release_result));
      }
      schedule_poll(now, draining);
    }
  }

  void DesktopHost::Impl::progress_presents(
    const std::chrono::steady_clock::time_point now, const bool draining)
  {
    for (std::size_t index{}; index < presents_.size(); ++index) {
      auto &record = presents_[index];
      if (record.use.phase != render::PresentUsePhase::PresentPending) continue;
      const auto status = vkGetFenceStatus(device_, record.fence);
      if (status == VK_NOT_READY) {
        schedule_poll(now, draining);
        continue;
      }
      if (status == VK_ERROR_DEVICE_LOST) {
        enter_fatal(FatalKind::DeviceLost,
                    vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                 status));
        return;
      }
      if (status != VK_SUCCESS) {
        enter_fatal(FatalKind::Terminal,
                    vulkan_error(core::AppErrorCode::RuntimeStateLost,
                                 status));
        schedule_poll(now, draining);
        continue;
      }

      const bool confirms_recovery = record.confirms_recovery;
      if (record.counts_as_presented) observe_performance(PerformanceCounter::PresentRetired);
      made_progress_ = true;
      if (confirms_recovery && recovery_episode_) {
        recovery_episode_->state =
          render::advance_graphics_recovery_episode(
            recovery_episode_->state,
            render::GraphicsRecoveryObservation::SuccessfulPresentRetired);
        recovery_episode_.reset();
        for (auto &retired : retired_swapchains_) {
          retired.recovery_candidate = false;
        }
        if (active_swapchain_) {
          active_swapchain_->recovery_candidate = false;
        }
      }
      if (!recycle_present({index, record.use.serial}, render::PresentUsePhase::PresentPending) &&
          (fatal_kind_ == FatalKind::DeviceLost || fatal_kind_ == FatalKind::Unexpected)) {
        return;
      }
    }
  }

  render::SwapchainOwnershipFacts DesktopHost::Impl::ownership(
    const VkSwapchainKHR swapchain) const noexcept
  {
    render::SwapchainOwnershipFacts facts;
    for (const auto &record : frames_) {
      if (record.phase == FramePhase::Free || record.swapchain != swapchain) {
        continue;
      }
      if (record.phase == FramePhase::ReleaseRetained) {
        facts.release_retained = true;
      }
      else {
        facts.native_image_use_pending = true;
      }
    }
    facts.present_fence_pending = std::ranges::any_of(
      presents_, [swapchain](const PresentRecord &record) noexcept {
        return record.use.phase == render::PresentUsePhase::PresentPending &&
               record.swapchain == swapchain;
      });
    return facts;
  }

  void DesktopHost::Impl::clear_release_retained(
    const VkSwapchainKHR swapchain) noexcept
  {
    for (auto &record : frames_) {
      if (record.swapchain != swapchain ||
          record.phase != FramePhase::ReleaseRetained) {
        continue;
      }
      recycle_frame(record);
      if (fatal_kind_ == FatalKind::DeviceLost || fatal_kind_ == FatalKind::Unexpected) return;
    }
  }

  void DesktopHost::Impl::progress_retired_swapchains()
  {
    auto current = retired_swapchains_.begin();
    while (current != retired_swapchains_.end()) {
      const auto handle = current->handle;
      if (!render::retired_swapchain_destroyable(ownership(handle))) {
        ++current;
        continue;
      }
      destroy_swapchain_state(*current);
      current = retired_swapchains_.erase(current);
      clear_release_retained(handle);
      if (fatal_kind_ == FatalKind::DeviceLost || fatal_kind_ == FatalKind::Unexpected) return;
      made_progress_ = true;
    }
  }

  void DesktopHost::Impl::progress_recreation(
    const std::chrono::steady_clock::time_point now)
  {
    progress_retired_swapchains();
    if (!swapchain_recreate_pending_ || stopping_controller_ ||
        fatal_kind_ != FatalKind::None) {
      return;
    }
    const auto requested_extent = framebuffer_extent();
    if (zero_extent(requested_extent) ||
        glfwGetWindowAttrib(window_, GLFW_ICONIFIED) == GLFW_TRUE) {
      return;
    }

    if (active_swapchain_) {
      const auto active_ownership = ownership(active_swapchain_->handle);
      if (active_ownership.native_image_use_pending) {
        schedule_poll(now, false);
        return;
      }
      if (active_ownership.release_retained) {
        enter_fatal(FatalKind::Terminal,
                    graphics_error(core::AppErrorCode::RuntimeStateLost));
        return;
      }
    }

    if (!recovery_episode_) {
      recovery_episode_ = RecoveryEpisode{.started_at = now};
    }
    const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
        now - recovery_episode_->started_at);
    const auto retry_after = controller_retry_after(
      recovery_episode_->state.attempt, elapsed);
    if (!retry_after) {
      enter_fatal(
        FatalKind::Terminal,
        recovery_episode_->last_error.value_or(
          graphics_error(core::AppErrorCode::RuntimeStateLost)));
      return;
    }
    if (*retry_after > 0ms) {
      earlier_deadline(graphics_deadline_, now + *retry_after);
      return;
    }

    recovery_episode_->state =
      render::advance_graphics_recovery_episode(
        recovery_episode_->state,
        render::GraphicsRecoveryObservation::ConstructionAttempted);
    const auto created = create_swapchain(requested_extent, true);
    if (created) {
      recovery_episode_->state =
        render::advance_graphics_recovery_episode(
          recovery_episode_->state,
          render::GraphicsRecoveryObservation::ReplacementConstructed);
      swapchain_recreate_pending_ = false;
      made_progress_ = true;
      return;
    }

    recovery_episode_->last_error = created.error();
    if (created.error().native_detail ==
        static_cast<std::int64_t>(VK_ERROR_DEVICE_LOST)) {
      enter_fatal(FatalKind::DeviceLost, created.error());
      return;
    }
    if (created.error().native_detail ==
        static_cast<std::int64_t>(VK_ERROR_SURFACE_LOST_KHR)) {
      enter_fatal(FatalKind::SurfaceLost, created.error());
      return;
    }

    const auto elapsed_after =
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - recovery_episode_->started_at);
    const auto next_retry_after = controller_retry_after(
      recovery_episode_->state.attempt, elapsed_after);
    if (!next_retry_after) {
      enter_fatal(FatalKind::Terminal, created.error());
      return;
    }
    earlier_deadline(
      graphics_deadline_,
      std::chrono::steady_clock::now() +
        render::strictly_future_retry_delay(*next_retry_after));
  }

  void DesktopHost::Impl::progress(
    const std::chrono::steady_clock::time_point now, const bool draining)
  {
    graphics_deadline_.reset();
    made_progress_ = false;
    if (fatal_kind_ == FatalKind::DeviceLost || fatal_kind_ == FatalKind::Unexpected) {
      return;
    }
    progress_frames(now, draining);
    if (fatal_kind_ == FatalKind::DeviceLost || fatal_kind_ == FatalKind::Unexpected) return;
    progress_presents(now, draining);
    if (fatal_kind_ == FatalKind::DeviceLost || fatal_kind_ == FatalKind::Unexpected) {
      return;
    }

    if (draining) {
      if (active_swapchain_ &&
          !ownership(active_swapchain_->handle).native_image_use_pending) {
        retire_active_swapchain();
        made_progress_ = true;
      }
      progress_retired_swapchains();
      if (!controller_drained()) {
        schedule_poll(now, true);
      }
      if (made_progress_) {
        note_progress(now);
      }
      else if (!no_progress_since_ && !controller_drained()) {
        no_progress_since_ = now;
      }
      return;
    }

    progress_recreation(now);
    if (made_progress_) {
      note_progress(now);
    }
  }

  void DesktopHost::Impl::render_controller(
    app::Application &application,
    const std::chrono::steady_clock::time_point now)
  {
    // A due visible frame consumes queued input even while native capacity is
    // occupied. The view borrows the current projection through this call; its
    // owning command values enter the application's next independently run step.
    if (performance_) {
      const auto observed = std::chrono::steady_clock::now();
      performance_->observe_first_ui(observed);
    }
    const auto ui_started = performance_ ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    float xscale{1.0F}, yscale{1.0F};
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    constexpr float logical_resize_margin{8.0F};
    const auto frame = controller_view_.render(
      application.view_state(),
      [this](app::AppCommand command) { emit(std::move(command)); },
      logical_resize_margin * std::max({1.0F, xscale, yscale}));
    ImGui::Render();
    if (performance_) {
      const auto finished = std::chrono::steady_clock::now();
      performance_->observe_counter(PerformanceOwner::Controller, PerformanceCounter::UiBuilds, 1, finished);
      performance_->observe_sample(PerformanceSample::UiBuild,
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - ui_started), finished);
    }
    const auto display = ImGui::GetIO().DisplaySize;
    window_interaction_->set_drag_region(
      display.x > 0.0F ? frame.drag_right / display.x : 0.0F,
      display.y > 0.0F ? frame.drag_bottom / display.y : 0.0F);
    ui_cadence_.did_build(std::chrono::steady_clock::now());
    if (!zero_extent(framebuffer_extent()) &&
        glfwGetWindowAttrib(window_, GLFW_ICONIFIED) != GLFW_TRUE &&
        fatal_kind_ == FatalKind::None) {
      attempt_present(*ImGui::GetDrawData(), now);
    }
    // Native iconify can synchronously queue focus/release callbacks. Acknowledge
    // this frame above so those newly observed inputs request a hidden sync.
    if (frame.minimize_requested && fatal_kind_ == FatalKind::None) {
      glfwIconifyWindow(window_);
    }
  }

  void DesktopHost::Impl::schedule_poll(
    const std::chrono::steady_clock::time_point now,
    const bool draining) noexcept
  {
    earlier_deadline(graphics_deadline_,
                     now + (draining ? drain_poll_interval
                                     : active_poll_interval));
  }

  void DesktopHost::Impl::note_progress(
    const std::chrono::steady_clock::time_point now) noexcept
  {
    if (!controller_drained()) {
      no_progress_since_ = now;
    }
    else {
      no_progress_since_.reset();
    }
  }

  void DesktopHost::Impl::enter_fatal(const FatalKind kind,
                                      const core::AppError error) noexcept
  {
    if (performance_) {
      performance_->observe_stop(PerformanceOwner::Controller, std::chrono::steady_clock::now());
    }
    const bool promote = fatal_kind_ == FatalKind::None ||
                         kind == FatalKind::DeviceLost ||
                         kind == FatalKind::Unexpected;
    if (promote) {
      fatal_kind_ = kind;
    }
    stopping_controller_ = true;
    if (promote) {
      const auto label = error.diagnostic_label();
      std::fprintf(stderr, "avb0795 controller failure: %.*s\n",
                   static_cast<int>(label.size()), label.data());
    }
  }

  bool DesktopHost::Impl::drain_stalled(
    const std::chrono::steady_clock::time_point now) const noexcept
  {
    return no_progress_since_ && !controller_drained() &&
           now - *no_progress_since_ >= drain_stalled_threshold;
  }

  bool DesktopHost::Impl::unsafe_controller_work() const noexcept
  {
    return fatal_kind_ == FatalKind::DeviceLost ||
           fatal_kind_ == FatalKind::Unexpected || font_texture_.upload_in_flight ||
           std::ranges::any_of(frames_, [](const auto &record) {
             return record.phase != FramePhase::Free;
           }) ||
           std::ranges::any_of(presents_, [](const auto &record) {
             return record.use.phase != render::PresentUsePhase::Free;
           });
  }

  bool DesktopHost::Impl::controller_drained() const noexcept
  {
    return !unsafe_controller_work() && !active_swapchain_ &&
           retired_swapchains_.empty();
  }

  void DesktopHost::Impl::retire_active_swapchain()
  {
    if (!active_swapchain_) {
      return;
    }
    retired_swapchains_.push_back(std::move(*active_swapchain_));
    active_swapchain_.reset();
  }

  VkExtent2D DesktopHost::Impl::framebuffer_extent() const noexcept
  {
    int width{};
    int height{};
    glfwGetFramebufferSize(window_, &width, &height);
    return {
      .width = width > 0 ? static_cast<std::uint32_t>(width) : 0U,
      .height = height > 0 ? static_cast<std::uint32_t>(height) : 0U,
    };
  }

  void DesktopHost::Impl::wait_until(
    const std::optional<std::chrono::steady_clock::time_point> deadline,
    const bool may_block) const noexcept
  {
    if (!may_block) {
      return;
    }
    if (!deadline) {
      glfwWaitEvents();
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (*deadline <= now) {
      return;
    }
    const auto timeout = std::chrono::duration<double>(*deadline - now).count();
    glfwWaitEventsTimeout(timeout);
  }

  void DesktopHost::Impl::drive_application_shutdown(
    app::Application &application, const bool controller_firewall)
  {
    // A failed controller is not permission to destroy the other resource graph.
    // Keep advancing its owning application's shutdown even when controller
    // graphics are behind the failure firewall and must no longer be touched.
    stopping_controller_ = true;
    application.shutdown();
    while (application.exit_disposition() ==
           core::ExitDisposition::Continue) {
      glfwPollEvents();
      const auto now = std::chrono::steady_clock::now();
      const bool graphics_allowed = !controller_firewall &&
        fatal_kind_ != FatalKind::DeviceLost && fatal_kind_ != FatalKind::Unexpected;
      if (graphics_allowed) {
        progress(now, true);
      }
      application.step(now);
      if (application.exit_disposition() != core::ExitDisposition::Continue) {
        break;
      }
      if (drain_stalled(now)) {
        no_progress_since_ =
          now - drain_stalled_threshold - std::chrono::milliseconds{1};
      }
      auto deadline = application.next_deadline();
      if (graphics_allowed && fatal_kind_ != FatalKind::DeviceLost &&
          fatal_kind_ != FatalKind::Unexpected &&
          !drain_stalled(now) && graphics_deadline_) {
        earlier_deadline(deadline, *graphics_deadline_);
      }
      wait_until(deadline, true);
    }
  }

  HostRunResult DesktopHost::Impl::run(
    app::Application &application) noexcept
  {
    try {
      while (application.exit_disposition() ==
               core::ExitDisposition::Continue &&
             fatal_kind_ == FatalKind::None) {
        glfwPollEvents();
        if (fatal_kind_ != FatalKind::None) break;
        const auto now = std::chrono::steady_clock::now();
        if (performance_) performance_->advance(now);
        observe_controller_visibility(now);
        progress(now, false);
        if (fatal_kind_ != FatalKind::None) {
          break;
        }
        if (performance_) observe_performance_metadata(std::chrono::steady_clock::now());
        application.step(now);
        if (application.exit_disposition() != core::ExitDisposition::Continue ||
            fatal_kind_ != FatalKind::None) break;
        const auto ui = ui_cadence_.decision(std::chrono::steady_clock::now(),
                                            interaction_active());
        if (ui.mode == detail::UiTickMode::Visible) {
          render_controller(application, now);
        } else if (ui.mode == detail::UiTickMode::HiddenSync) {
          detail::synchronize_hidden_imgui_input([] {
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
          });
          ui_cadence_.did_build(std::chrono::steady_clock::now());
          observe_performance(PerformanceCounter::HiddenSyncs);
        } else {
          observe_performance(PerformanceCounter::GraphicsOnlyWakes);
        }
        if (application.exit_disposition() != core::ExitDisposition::Continue ||
            fatal_kind_ != FatalKind::None) {
          break;
        }
        // A UI action may synchronously iconify the window. Observe its resulting
        // visibility before choosing a wait so the pending hidden input is drained.
        const auto after_ui = std::chrono::steady_clock::now();
        observe_controller_visibility(after_ui);
        const auto next_ui = ui_cadence_.decision(after_ui, interaction_active());
        const auto wait = detail::choose_host_wait(
          std::exchange(command_emitted_, false), application.next_deadline(),
          graphics_deadline_, next_ui.next_deadline);
        if (wait.immediate_reentry) continue;
        wait_until(wait.deadline, true);
      }

      if (application.exit_disposition() ==
          core::ExitDisposition::AbandonProcess) {
        return {.exit_code = abandon_exit_code, .abandon_process = true};
      }

      if (fatal_kind_ != FatalKind::None) {
        const bool controller_firewall =
          fatal_kind_ == FatalKind::DeviceLost ||
          fatal_kind_ == FatalKind::Unexpected;
        drive_application_shutdown(application, controller_firewall);
        if (application.exit_disposition() ==
              core::ExitDisposition::AbandonProcess ||
            fatal_kind_ == FatalKind::DeviceLost ||
            fatal_kind_ == FatalKind::Unexpected) {
          return {.exit_code = abandon_exit_code, .abandon_process = true};
        }
      }

      stopping_controller_ = true;
      no_progress_since_ = std::chrono::steady_clock::now();
      while (!controller_drained()) {
        glfwPollEvents();
        if (fatal_kind_ == FatalKind::DeviceLost ||
            fatal_kind_ == FatalKind::Unexpected) {
          return {.exit_code = abandon_exit_code, .abandon_process = true};
        }
        const auto now = std::chrono::steady_clock::now();
        progress(now, true);
        if (fatal_kind_ == FatalKind::DeviceLost ||
            fatal_kind_ == FatalKind::Unexpected || drain_stalled(now)) {
          return {.exit_code = abandon_exit_code, .abandon_process = true};
        }
        if (controller_drained()) {
          break;
        }
        wait_until(graphics_deadline_, true);
      }
      return {.exit_code = fatal_kind_ == FatalKind::None
                             ? 0
                             : controller_failure_exit_code,
              .abandon_process = false};
    } catch (const BackendVulkanFailure &failure) {
      enter_fatal(
        FatalKind::Unexpected,
        vulkan_error(core::AppErrorCode::RuntimeStateLost, failure.result));
      drive_application_shutdown(application, true);
      return {.exit_code = abandon_exit_code, .abandon_process = true};
    } catch (...) {
      enter_fatal(FatalKind::Unexpected,
                  graphics_error(core::AppErrorCode::RuntimeStateLost));
      drive_application_shutdown(application, true);
      return {.exit_code = abandon_exit_code, .abandon_process = true};
    }
  }

  void DesktopHost::Impl::destroy_frame_record(FrameRecord &record) noexcept
  {
    if (device_ != VK_NULL_HANDLE) {
      if (record.acquire_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, record.acquire_semaphore, nullptr);
        observe_performance(PerformanceCounter::SemaphoresDestroyed);
      }
      if (record.acquire_fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, record.acquire_fence, nullptr);
        observe_performance(PerformanceCounter::FencesDestroyed);
      }
      if (record.submission_fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, record.submission_fence, nullptr);
        observe_performance(PerformanceCounter::FencesDestroyed);
      }
      if (record.command_buffer != VK_NULL_HANDLE &&
          command_pool_ != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, command_pool_, 1,
                             &record.command_buffer);
        observe_performance(PerformanceCounter::CommandBuffersDestroyed);
      }
    }
    record = {};
  }

  void DesktopHost::Impl::destroy_present_record(
    PresentRecord &record) noexcept
  {
    if (device_ != VK_NULL_HANDLE) {
      if (record.wait_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, record.wait_semaphore, nullptr);
        observe_performance(PerformanceCounter::SemaphoresDestroyed);
      }
      if (record.fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, record.fence, nullptr);
        observe_performance(PerformanceCounter::FencesDestroyed);
      }
    }
    record = {};
  }

  void DesktopHost::Impl::destroy_swapchain_state(
    SwapchainState &state) noexcept
  {
    if (device_ != VK_NULL_HANDLE) {
      for (const auto framebuffer : state.framebuffers) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
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
    }
    state = {};
  }

  void DesktopHost::Impl::destroy_font_staging() noexcept
  {
    if (device_ != VK_NULL_HANDLE) {
      if (font_texture_.staging_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, font_texture_.staging_buffer, nullptr);
      }
      if (font_texture_.staging_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, font_texture_.staging_memory, nullptr);
      }
    }
    font_texture_.staging_buffer = VK_NULL_HANDLE;
    font_texture_.staging_memory = VK_NULL_HANDLE;
  }

  void DesktopHost::Impl::destroy_font_texture() noexcept
  {
    destroy_font_staging();
    if (device_ != VK_NULL_HANDLE) {
      if (font_texture_.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, font_texture_.view, nullptr);
      }
      if (font_texture_.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, font_texture_.image, nullptr);
      }
      if (font_texture_.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, font_texture_.memory, nullptr);
      }
    }
    font_texture_ = {};
  }

  void DesktopHost::Impl::cleanup_partial() noexcept
  {
    // This routine releases dependencies; it does not establish completion.
    // Callers admit only never-submitted startup state or already-retired work.
    // Remove the atlas descriptor/backend/context while their device and borrowed
    // font storage still exist, then destroy images, pools, device and surface.
    if (display_callbacks_installed_) {
      glfwSetFramebufferSizeCallback(window_, previous_framebuffer_callback_);
      glfwSetWindowContentScaleCallback(window_, previous_content_scale_callback_);
      glfwSetWindowRefreshCallback(window_, previous_refresh_callback_);
      glfwSetWindowIconifyCallback(window_, previous_iconify_callback_);
      display_callbacks_installed_ = false;
    }
    if (imgui_vulkan_initialized_) {
      if (font_texture_.descriptor != VK_NULL_HANDLE) {
        if (auto *const texture_data = ImGui::GetIO().Fonts->TexData;
            texture_data != nullptr) {
          texture_data->SetTexID(ImTextureID_Invalid);
          texture_data->BackendUserData = nullptr;
        }
        ImGui_ImplVulkan_RemoveTexture(font_texture_.descriptor);
        font_texture_.descriptor = VK_NULL_HANDLE;
      }
      ImGui_ImplVulkan_Shutdown();
      imgui_vulkan_initialized_ = false;
    }
    if (imgui_glfw_initialized_) {
      ImGui_ImplGlfw_Shutdown();
      imgui_glfw_initialized_ = false;
    }
    if (imgui_context_created_) {
      ImGui::DestroyContext();
      imgui_context_created_ = false;
    }

    destroy_font_texture();
    for (auto &record : frames_) {
      destroy_frame_record(record);
    }
    frames_.clear();
    for (auto &record : presents_) {
      destroy_present_record(record);
    }
    presents_.clear();
    if (active_swapchain_) {
      destroy_swapchain_state(*active_swapchain_);
      active_swapchain_.reset();
    }
    for (auto &state : retired_swapchains_) {
      destroy_swapchain_state(state);
    }
    retired_swapchains_.clear();

    if (device_ != VK_NULL_HANDLE) {
      if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
      }
      if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
      }
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

  DesktopHost::DesktopHost(std::unique_ptr<Impl> impl) noexcept
      : impl_{std::move(impl)}
  {
  }

  DesktopHost::~DesktopHost() = default;
  DesktopHost::DesktopHost(DesktopHost &&) noexcept = default;
  DesktopHost &DesktopHost::operator=(DesktopHost &&) noexcept = default;

  std::expected<DesktopHost, core::AppError> DesktopHost::create(
    CommandSink command_sink, ControllerWindowCreated window_created, std::vector<std::byte> font_data,
    diagnostics::PerformanceReport *performance)
  {
    if (!command_sink || !window_created) {
      return std::unexpected{
        graphics_error(core::AppErrorCode::InvalidValue)};
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow *const window = glfwCreateWindow(
      controller_width, controller_height, "avb0795", nullptr, nullptr);
    // GLFW hints are process-global; later replica creation starts visible.
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    if (window == nullptr) {
      const char *description{};
      const int detail = glfwGetError(&description);
      static_cast<void>(description);
      return std::unexpected{graphics_error(
        core::AppErrorCode::RuntimeConstructionFailure,
        static_cast<std::int64_t>(detail))};
    }

    try {
      auto setup = window_created(window);
      if (!setup) {
        glfwDestroyWindow(window);
        return std::unexpected{setup.error()};
      }
      auto impl =
        std::make_unique<Impl>(window, std::move(command_sink), std::move(*setup), std::move(font_data), performance);
      glfwShowWindow(window);
      return DesktopHost{std::move(impl)};
    } catch (const BackendVulkanFailure &failure) {
      glfwDestroyWindow(window);
      return std::unexpected{vulkan_error(
        core::AppErrorCode::RuntimeConstructionFailure, failure.result)};
    } catch (const core::AppError &error) {
      glfwDestroyWindow(window);
      return std::unexpected{error};
    } catch (...) {
      glfwDestroyWindow(window);
      return std::unexpected{graphics_error(
        core::AppErrorCode::RuntimeConstructionFailure)};
    }
  }

  core::CaptureExclusionStatus
  DesktopHost::controller_exclusion() const noexcept
  {
    return impl_ ? impl_->controller_exclusion()
                 : core::CaptureExclusionStatus::NotAttempted;
  }

  void DesktopHost::wake() const noexcept
  {
    if (impl_) {
      impl_->wake();
    }
  }

  bool DesktopHost::try_record_hotkey(const core::HotkeyChord chord) noexcept
  {
    return impl_ && impl_->try_record_hotkey(chord);
  }

  HostRunResult DesktopHost::run(app::Application &application) noexcept
  {
    return impl_ ? impl_->run(application)
                 : HostRunResult{.exit_code = abandon_exit_code,
                                 .abandon_process = true};
  }

}    // namespace avb::desktop
