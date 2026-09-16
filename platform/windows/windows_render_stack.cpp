/// @file
/// @brief Main-thread Windows producer/Vulkan renderer composition and transport policy.
#include "platform/windows/windows_render_stack.hpp"
#include "avb/diagnostics/performance_report.hpp"
#include "platform/windows/detail/graphics_construction_guard.hpp"
#include "platform/windows/overlay_window.hpp"
#include "platform/windows/windows_captured_frame.hpp"
#include "platform/windows/windows_frame_bridge.hpp"
#include "render/vulkan_renderer.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace avb::render {
  using diagnostics::PerformanceCounter;
  using diagnostics::PerformanceOwner;
  namespace {
    constexpr std::size_t source_slot_count = VulkanRenderer::source_slot_capacity;
    [[nodiscard]] constexpr core::AppError graphics_error(
      const core::AppErrorCode code,
      const std::optional<std::int64_t> detail = std::nullopt) noexcept
    { return {.code = code, .native_detail = detail}; }
    [[nodiscard]] bool valid_allocation(const core::Extent extent) noexcept
    {
      if (extent.width == 0 || extent.height == 0) return false;
      constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
      return static_cast<std::size_t>(extent.width) <= maximum / extent.height / 4;
    }
    template <typename Value>
    void earlier_deadline(std::optional<Value> &current, const Value candidate) noexcept
    {
      if (!current || candidate < *current) current = candidate;
    }
  }    // namespace

  /// @brief Compose platform transport with one concrete shared renderer.
  /// The facade owns transport choice, producer provenance and the transaction
  /// that installs both rings. Slot phase, descriptors, WSI and redraw content
  /// are addressed through renderer operations rather than mirrored here.
  class WindowsRenderStack::Impl final {
  public:
    Impl(GLFWwindow *window, std::uint64_t generation, core::Extent framebuffer, WindowsBridgePreference preference, diagnostics::PerformanceReport *performance, std::uint64_t observation_instance);
    ~Impl() noexcept;
    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    ID3D11Device &capture_device() const noexcept { return frame_bridge_->capture_device(); }
    bool ready_for_frame() const noexcept
    {
      return accepting_frames_ && !terminal_error_ && !d3_device_removed_ &&
             renderer_->ready_for_frame();
    }
    void submit(platform::windows::WindowsCapturedFrame &&frame);
    void set_presentation(ReplicaPresentation presentation) noexcept { renderer_->set_presentation(presentation); }
    void set_performance_fps(core::FpsCap fps) noexcept { performance_fps_ = fps; }
    bool has_redraw_work() const noexcept
    {
      return !draining_ && !terminal_error_ && !d3_device_removed_ &&
             renderer_->has_redraw_work();
    }
    void request_redraw() noexcept { renderer_->request_redraw(); }
    RenderTickResult tick(core::Extent framebuffer, std::chrono::steady_clock::time_point now) noexcept;
    bool render_allocation_idle() const noexcept
    {
      return !frame_bridge_->has_pending_inputs() && renderer_->render_allocation_idle();
    }
    std::expected<void, core::AppError> rebuild_capture_slots(
      std::uint64_t generation, core::Extent allocation, DXGI_FORMAT format);
    void resize(core::Extent framebuffer) noexcept { renderer_->resize(framebuffer); }
    void begin_drain() noexcept;
    void begin_device_recovery_drain(DeviceLossKind kind) noexcept;
    std::uint64_t generation() const noexcept { return generation_; }
    BridgeCapabilityFacts bridge_capability_facts() const noexcept { return bridge_facts_; }
    std::optional<BridgeSelection> bridge_selection() const noexcept { return bridge_selection_; }
    core::BridgeMode active_bridge_mode() const noexcept { return active_bridge_mode_; }
    std::uint64_t gpu_interop_frames_submitted() const noexcept { return renderer_->gpu_interop_frames_submitted(); }
    std::optional<BridgeSelection> take_bridge_mode_change() noexcept
    {
      return std::exchange(bridge_change_pending_, std::nullopt);
    }

  private:
    void observe_performance(PerformanceCounter counter, std::uint64_t value = 1) noexcept;
    void observe_performance_metadata(std::chrono::steady_clock::time_point now) noexcept;
    std::expected<void, core::AppError> destroy_capture_slots() noexcept;
    std::expected<void, core::AppError> start_source_ring(
      std::uint64_t generation, core::Extent allocation, DXGI_FORMAT format) noexcept;
    std::expected<void, core::AppError> create_capture_slot(SlotKey key);
    void request_interop_fallback() noexcept;
    void progress_interop_fallback(RenderTickResult &result);
    void progress_d3_copy(RenderTickResult &result, std::chrono::steady_clock::time_point now);
    void mark_d3_device_removed(RenderTickResult &result) noexcept;
    void mark_vulkan_device_lost(RenderTickResult &result) noexcept;
    void close_admission() noexcept
    {
      accepting_frames_ = false;
      renderer_->set_source_admission(false);
    }
    struct BootstrapOperations final {
      Impl &owner;
      bool detach_renderer_references() noexcept
      {
        if (owner.renderer_) owner.renderer_->detach_for_shutdown();
        return true;
      }
      void release_bridge_resources() noexcept { owner.frame_bridge_.reset(); }
      void release_renderer_resources() noexcept { owner.renderer_.reset(); }
    };
    struct RingInstallOperations final {
      Impl &owner;
      std::uint64_t serial;
      bool bridge_started{true};
      bool detach_renderer_references() noexcept { return owner.renderer_->detach_ring(serial).has_value(); }
      void release_bridge_resources() noexcept
      {
        if (bridge_started && !owner.frame_bridge_->release_ring(serial)) std::terminate();
      }
      void release_renderer_resources() noexcept
      {
        owner.renderer_->release_ring_resources();
        owner.ring_serial_ = 0;
        owner.capture_allocation_ = {};
        owner.capture_format_ = DXGI_FORMAT_UNKNOWN;
      }
    };

    // Explicit final teardown detaches references, releases imports, then device.
    // Declaration order also keeps this relationship during member unwinding.
    std::unique_ptr<VulkanRenderer> renderer_;
    std::unique_ptr<platform::windows::WindowsFrameBridge> frame_bridge_;
    diagnostics::PerformanceReport *performance_{};
    std::uint64_t observation_instance_{};
    core::Extent performance_content_{};
    core::FpsCap performance_fps_{core::FpsCap::Fps60};
    std::uint64_t generation_{};
    WindowsBridgePreference bridge_preference_{};
    std::uint64_t ring_serial_{};
    core::Extent capture_allocation_{};
    DXGI_FORMAT capture_format_{DXGI_FORMAT_UNKNOWN};
    BridgeCapabilityFacts bridge_facts_{};
    std::optional<BridgeSelection> bridge_selection_;
    std::optional<BridgeSelection> bridge_change_pending_;
    core::BridgeMode active_bridge_mode_{core::BridgeMode::CpuCopy};
    bool bridge_capabilities_collected_{};
    bool interop_fallback_pending_{};
    bool accepting_frames_{true};
    bool draining_{};
    bool d3_device_removed_{};
    std::optional<core::AppError> terminal_error_;
    std::uint64_t captured_frames_{};
    std::uint64_t dropped_frames_{};
  };

  WindowsRenderStack::Impl::Impl(GLFWwindow *window,
                                 const std::uint64_t generation,
                                 const core::Extent framebuffer,
                                 const WindowsBridgePreference preference,
                                 diagnostics::PerformanceReport *performance,
                                 const std::uint64_t observation_instance)
      : performance_{performance}, observation_instance_{observation_instance}, generation_{generation}, bridge_preference_{preference}
  {
    BootstrapOperations operations{*this};
    platform::windows::detail::GraphicsConstructionGuard guard{operations};
    frame_bridge_ = std::make_unique<platform::windows::WindowsFrameBridge>(
      platform::windows::BridgeResourceObserver{
        .context = this,
        .observe = [](void *context, const platform::windows::BridgeResourceEvent event) noexcept {
          auto &owner = *static_cast<Impl *>(context);
          if (owner.renderer_) {
            owner.renderer_->observe_external_semaphore(
              event == platform::windows::BridgeResourceEvent::SemaphoreCreated);
          }
        },
      });
    renderer_ = std::make_unique<VulkanRenderer>(
      window, generation, framebuffer, frame_bridge_->startup_request(),
      performance, observation_instance);
    frame_bridge_->attach_vulkan_device(renderer_->device_borrow());
    bridge_facts_ = frame_bridge_->capability_facts();
    bridge_capabilities_collected_ = true;
    guard.disarm();
  }

  WindowsRenderStack::Impl::~Impl() noexcept
  {
    if (frame_bridge_->has_pending_inputs()) std::terminate();
    renderer_->detach_for_shutdown();
    frame_bridge_.reset();
    renderer_.reset();
  }

  void WindowsRenderStack::Impl::observe_performance(
    const PerformanceCounter counter, const std::uint64_t value) noexcept
  {
    if (!performance_) return;
    const auto now = std::chrono::steady_clock::now();
    performance_->observe_runtime({observation_instance_, generation_}, now);
    performance_->observe_counter(PerformanceOwner::Replica, counter, value, now);
  }

  void WindowsRenderStack::Impl::observe_performance_metadata(
    const std::chrono::steady_clock::time_point now) noexcept
  {
    constexpr std::array reasons{
      "none", "adapter_luid_unavailable_or_mismatch", "external_image_unsupported",
      "required_d3d11_interfaces_unavailable", "shared_fence_or_timeline_unavailable",
      "timeline_import_unavailable", "win32_import_procedure_unavailable",
      "compatible_memory_type_unavailable", "interop_operation_failure"};
    renderer_->observe_metadata({
                                  .os = "Windows",
                                  .bridge = active_bridge_mode_ == core::BridgeMode::GpuInterop ? "gpu_interop" : "cpu_copy",
                                  .bridge_reason = bridge_selection_ ? reasons[static_cast<std::size_t>(bridge_selection_->reason)] : "pending",
                                  .requested_bridge = bridge_preference_ == WindowsBridgePreference::ForceCpuBaseline ? "cpu" : "auto",
                                  .content = performance_content_,
                                  .fps = performance_fps_,
                                },
                                now);
  }

  std::expected<void, core::AppError>
  WindowsRenderStack::Impl::destroy_capture_slots() noexcept
  {
    if (frame_bridge_->has_pending_inputs()) std::terminate();
    if (ring_serial_ != 0) {
      if (auto detached = renderer_->detach_ring(ring_serial_); !detached) return detached;
      if (!frame_bridge_->release_ring(ring_serial_)) std::terminate();
    }
    renderer_->release_ring_resources();
    ring_serial_ = 0;
    capture_allocation_ = {};
    capture_format_ = DXGI_FORMAT_UNKNOWN;
    return {};
  }

  std::expected<void, core::AppError>
  WindowsRenderStack::Impl::start_source_ring(const std::uint64_t generation,
                                              const core::Extent allocation,
                                              const DXGI_FORMAT format) noexcept
  {
    const auto started = renderer_->begin_ring(allocation);
    if (!started) return std::unexpected{started.error()};
    ring_serial_ = *started;
    capture_allocation_ = allocation;
    capture_format_ = format;
    RingInstallOperations operations{*this, ring_serial_, false};
    platform::windows::detail::GraphicsConstructionGuard guard{operations};
    if (auto installed = frame_bridge_->start_ring(ring_serial_, generation, allocation, format); !installed) {
      return installed;
    }
    guard.disarm();
    return {};
  }

  std::expected<void, core::AppError>
  WindowsRenderStack::Impl::create_capture_slot(const SlotKey key)
  {
    if (auto created = frame_bridge_->create_cpu_slot(key); !created) return created;
    const auto created = renderer_->create_cpu_slot(key);
    if (!created && created.error().native_detail ==
                      static_cast<std::int64_t>(VK_ERROR_DEVICE_LOST)) renderer_->note_device_loss();
    return created;
  }

  std::expected<void, core::AppError>
  WindowsRenderStack::Impl::rebuild_capture_slots(const std::uint64_t generation,
                                                  const core::Extent allocation,
                                                  const DXGI_FORMAT format)
  {
    if (generation == 0 || !valid_allocation(allocation) ||
        format != DXGI_FORMAT_B8G8R8A8_UNORM || !render_allocation_idle() ||
        renderer_->vulkan_device_lost() || d3_device_removed_) {
      return std::unexpected{graphics_error(core::AppErrorCode::InvalidValue)};
    }
    const bool generation_changed = generation != generation_;
    renderer_->discard_ready_inputs();
    // The old ring is discarded before constructing replacements. The caller's
    // capture barrier must remain closed on failure; there is no old-ring rollback.
    if (auto detached = destroy_capture_slots(); !detached) {
      return detached;
    }
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
      frame_bridge_->refresh_interop_capabilities();
      bridge_facts_ = frame_bridge_->capability_facts();
    }
    const bool probe_interop = should_probe_interop_generation(
      interop_prefix_available(), bridge_selection_, generation_, generation);
    const bool attempt_interop =
      bridge_preference_ == WindowsBridgePreference::Automatic && probe_interop;

    bool interop_operation_failed{};
    if (probe_interop) {
      if (auto started = start_source_ring(generation, allocation, format); !started) {
        return started;
      }
      RingInstallOperations operations{*this, ring_serial_};
      platform::windows::detail::GraphicsConstructionGuard guard{operations};
      const auto probed = frame_bridge_->probe_interop_ring(ring_serial_);
      if (!probed) {
        return std::unexpected{probed.error()};
      }
      interop_operation_failed = probed->operation_failed;
      bridge_facts_ = frame_bridge_->capability_facts();
      if (attempt_interop && bridge_facts_.compatible_memory_type_available &&
          !interop_operation_failed) {
        for (std::uint32_t index{}; index < source_slot_count; ++index) {
          const SlotKey key{ring_serial_, index};
          const auto finished = frame_bridge_->finish_interop_slot(key);
          if (!finished) {
            interop_operation_failed = true;
            break;
          }
          if (auto registered = renderer_->register_external_image(key, *finished);
              !registered) {
            interop_operation_failed = true;
            break;
          }
        }
      }
      if (attempt_interop && bridge_facts_.compatible_memory_type_available &&
          !interop_operation_failed && !frame_bridge_->ensure_interop_timeline()) {
        interop_operation_failed = true;
      }
      if (attempt_interop && bridge_facts_.compatible_memory_type_available &&
          !interop_operation_failed) {
        if (auto published = renderer_->publish_ring(ring_serial_, generation); !published) return published;
        guard.disarm();
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
      if (auto started = start_source_ring(generation, allocation, format); !started) {
        return started;
      }
      RingInstallOperations operations{*this, ring_serial_};
      platform::windows::detail::GraphicsConstructionGuard guard{operations};
      for (std::uint32_t index{}; index < source_slot_count; ++index) {
        if (auto created = create_capture_slot({ring_serial_, index}); !created) {
          return created;
        }
      }
      frame_bridge_->release_interop_support();
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
      if (auto published = renderer_->publish_ring(ring_serial_, generation); !published) return published;
      guard.disarm();
    }
    generation_ = generation;
    return {};
  }

  void WindowsRenderStack::Impl::submit(
    platform::windows::WindowsCapturedFrame &&frame)
  {
    const auto content = frame.content_size();
    const auto key = ready_for_frame() ? renderer_->reserve_source_slot() : std::nullopt;
    if (!key) {
      ++dropped_frames_;
      observe_performance(PerformanceCounter::Dropped);
      throw graphics_error(core::AppErrorCode::InvalidValue);
    }

    const FrameStamp stamp{captured_frames_ + 1, frame.capture_timestamp()};
    const auto submitted = [&]() {
      try {
        return frame_bridge_->submit(*key, std::move(frame), stamp);
      } catch (...) {
        // Texture projection throws only before adoption/native enqueue. Cancel
        // while the old retained metadata and completed wait are still intact.
        if (!renderer_->cancel_source_reservation(*key)) {
          std::terminate();
        }
        throw;
      }
    }();
    if (!submitted) {
      if (!renderer_->cancel_source_reservation(*key)) {
        std::terminate();
      }
      if (submitted.error().code == core::AppErrorCode::InvalidValue) {
        ++dropped_frames_;
        observe_performance(PerformanceCounter::Dropped);
      }
      throw submitted.error();
    }
    performance_content_ = content;
    if (submitted->d3_copy_bytes != 0) {
      observe_performance(PerformanceCounter::D3CopyBytes, submitted->d3_copy_bytes);
    }
    if (submitted->count_as_dropped) {
      ++dropped_frames_;
      observe_performance(PerformanceCounter::Dropped);
    }
    if (submitted->fallback_requested) {
      request_interop_fallback();
    }
    if (submitted->d3_device_removed) {
      d3_device_removed_ = true;
      close_admission();
    }
    if (submitted->disposition ==
        platform::windows::BridgeSubmitDisposition::SafeDiscard) {
      if (!renderer_->discard_completed_source(*key)) {
        std::terminate();
      }
      return;
    }
    ++captured_frames_;
    observe_performance(PerformanceCounter::Admitted);
  }

  void WindowsRenderStack::Impl::progress_interop_fallback(
    RenderTickResult &result)
  {
    if (!interop_fallback_pending_ || renderer_->vulkan_device_lost() || d3_device_removed_) {
      return;
    }
    renderer_->discard_ready_inputs();
    const auto fallback = select_interop_fallback(
      render_allocation_idle() && result.presentations_in_flight == 0
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
    if (auto detached = destroy_capture_slots(); !detached) {
      interop_fallback_pending_ = false;
      throw core::AppError{
        .code = core::AppErrorCode::RuntimeStateLost,
        .native_detail = detached.error().native_detail,
      };
    }
    if (auto started = start_source_ring(generation_, allocation, format); !started) {
      interop_fallback_pending_ = false;
      throw started.error();
    }
    RingInstallOperations operations{*this, ring_serial_};
    platform::windows::detail::GraphicsConstructionGuard guard{operations};
    for (std::uint32_t index{}; index < source_slot_count; ++index) {
      if (auto created = create_capture_slot({ring_serial_, index}); !created) {
        const auto failure = created.error();
        interop_fallback_pending_ = false;
        const bool d3_lost = frame_bridge_->d3_device_removed();
        const bool vulkan_lost = renderer_->vulkan_device_lost();
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
    frame_bridge_->release_interop_support();
    active_bridge_mode_ = core::BridgeMode::CpuCopy;
    if (!bridge_selection_ || *bridge_selection_ != *fallback) {
      bridge_change_pending_ = *fallback;
    }
    bridge_selection_ = *fallback;
    interop_fallback_pending_ = false;
    if (auto published = renderer_->publish_ring(ring_serial_, generation_); !published) throw published.error();
    accepting_frames_ = !draining_ && !terminal_error_;
    renderer_->set_source_admission(accepting_frames_);
    result.made_progress = true;
    guard.disarm();
  }

  void WindowsRenderStack::Impl::progress_d3_copy(
    RenderTickResult &result, const std::chrono::steady_clock::time_point now)
  {
    const auto progress = frame_bridge_->progress(
      now, draining_ || renderer_->vulkan_device_lost() || interop_fallback_pending_,
      [this, &result](const SlotKey key, const CpuRowsView rows,
                      const FrameStamp stamp) {
        const auto completed = renderer_->complete_cpu_input(key, rows, stamp);
        if (completed) {
          result.made_progress = true;
        }
        else if (completed.error().native_detail ==
                 static_cast<std::int64_t>(VK_ERROR_DEVICE_LOST)) {
          mark_vulkan_device_lost(result);
        }
        return completed;
      });
    result.made_progress = result.made_progress || progress.made_progress;
    if (progress.next_deadline) {
      earlier_deadline(result.next_deadline, *progress.next_deadline);
    }
    if (progress.d3_device_removed) {
      mark_d3_device_removed(result);
    }
    if (progress.fallback_requested) {
      request_interop_fallback();
    }
    // Native failures retain D3 provenance even if a numeric HRESULT happens
    // to overlap a Vulkan result. Only consumer failures came from upload work.
    if (progress.terminal_error) {
      terminal_error_ = progress.terminal_error;
      close_admission();
      result.terminal_error = terminal_error_;
    }
    for (std::size_t index{}; index < progress.completion_count; ++index) {
      const auto &completion = progress.completions[index];
      using platform::windows::BridgeCompletionKind;
      const auto failed_transition = [this, &result](const core::AppError error) {
        terminal_error_ = error;
        close_admission();
        result.terminal_error = terminal_error_;
      };
      // CPU readiness was committed inside the synchronous mapped-row callback.
      // Every other event still owns the matching ProducerPending reservation.
      switch (completion.kind) {
      case BridgeCompletionKind::ExternalReady: {
        const auto completed = renderer_->complete_external_input(
          completion.key, completion.content, completion.stamp,
          completion.external_wait);
        if (!completed) {
          // The event already proves producer completion. Invalid publication
          // must consume its reservation without restoring superseded redraw.
          static_cast<void>(renderer_->discard_completed_source(completion.key));
          failed_transition(completed.error());
        }
        break;
      }
      case BridgeCompletionKind::CpuConsumed:
        // The synchronous consumer already committed upload and readiness.
        break;
      case BridgeCompletionKind::SafeDiscard:
      case BridgeCompletionKind::CpuConsumerFailed:
        if (const auto discarded = renderer_->discard_completed_source(completion.key);
            !discarded) {
          failed_transition(discarded.error());
          break;
        }
        if (completion.count_as_dropped) {
          ++dropped_frames_;
          observe_performance(PerformanceCounter::Dropped);
        }
        if (completion.error) {
          if (completion.kind == BridgeCompletionKind::CpuConsumerFailed &&
              completion.error->native_detail ==
                static_cast<std::int64_t>(VK_ERROR_DEVICE_LOST)) {
            mark_vulkan_device_lost(result);
          }
          else {
            terminal_error_ = completion.error;
            close_admission();
            result.terminal_error = terminal_error_;
          }
        }
        break;
      }
    }
  }


  void WindowsRenderStack::Impl::request_interop_fallback() noexcept
  {
    if (active_bridge_mode_ != core::BridgeMode::GpuInterop || interop_fallback_pending_) return;
    close_admission();
    interop_fallback_pending_ = true;
  }

  void WindowsRenderStack::Impl::mark_vulkan_device_lost(RenderTickResult &result) noexcept
  {
    renderer_->note_device_loss();
    close_admission();
    result.vulkan_device_lost = true;
    result.device_loss = DeviceLossKind::VulkanDeviceLost;
  }

  void WindowsRenderStack::Impl::mark_d3_device_removed(RenderTickResult &result) noexcept
  {
    d3_device_removed_ = true;
    frame_bridge_->note_d3_device_removed();
    close_admission();
    result.d3_device_removed = true;
    result.device_loss = renderer_->vulkan_device_lost() ? DeviceLossKind::VulkanDeviceLost
                                                         : DeviceLossKind::D3DeviceRemoved;
  }

  void WindowsRenderStack::Impl::begin_drain() noexcept
  {
    close_admission();
    draining_ = true;
    renderer_->begin_drain();
  }

  void WindowsRenderStack::Impl::begin_device_recovery_drain(const DeviceLossKind kind) noexcept
  {
    close_admission();
    draining_ = true;
    if (kind == DeviceLossKind::D3DeviceRemoved) {
      d3_device_removed_ = true;
      frame_bridge_->note_d3_device_removed();
    }
    renderer_->begin_recovery_drain(kind == DeviceLossKind::VulkanDeviceLost ||
                                    renderer_->vulkan_device_lost());
  }

  RenderTickResult WindowsRenderStack::Impl::tick(
    const core::Extent framebuffer,
    const std::chrono::steady_clock::time_point now) noexcept
  {
    // This order keeps already-completed producer waits available to the same
    // pass while preserving independent presentation and submission proof.
    auto graphics = renderer_->begin_tick(framebuffer, now);
    if (performance_) observe_performance_metadata(std::chrono::steady_clock::now());
    RenderTickResult result;
    bool interrupted{};
    try {
      progress_d3_copy(result, now);
      renderer_->retire_submissions(graphics, now);
      result.presentations_in_flight = graphics.presentations_in_flight;
      progress_interop_fallback(result);
    } catch (const core::AppError &error) {
      interrupted = true;
      if (renderer_->vulkan_device_lost()) {
        mark_vulkan_device_lost(result);
      }
      else {
        // Vulkan operations latch their own loss. Producer HRESULTs reach this
        // path as opaque errors and are never classified by numeric coincidence.
        terminal_error_ = error;
        close_admission();
      }
    } catch (const std::bad_alloc &) {
      interrupted = true;
      terminal_error_ = graphics_error(core::AppErrorCode::RuntimeStateLost, E_OUTOFMEMORY);
      close_admission();
    } catch (...) {
      interrupted = true;
      terminal_error_ = graphics_error(core::AppErrorCode::RuntimeStateLost);
      close_admission();
    }
    renderer_->finish_tick(graphics, now,
                           !interrupted && !draining_ && !terminal_error_ && !d3_device_removed_ &&
                             !interop_fallback_pending_,
                           !interrupted && (draining_ || !terminal_error_));

    result.surface_recovery_started = graphics.surface_recovery_started;
    result.surface_recovery_completed = graphics.surface_recovery_completed;
    result.vulkan_device_lost = renderer_->vulkan_device_lost();
    result.d3_device_removed = d3_device_removed_;
    if (result.vulkan_device_lost)
      result.device_loss = DeviceLossKind::VulkanDeviceLost;
    else if (d3_device_removed_)
      result.device_loss = DeviceLossKind::D3DeviceRemoved;
    else
      result.device_loss.reset();
    result.terminal_error = terminal_error_ ? terminal_error_ : graphics.terminal_error;
    result.made_progress = result.made_progress || graphics.made_progress;
    result.drain_complete = draining_ && !frame_bridge_->has_pending_inputs() && graphics.drain_complete;
    result.cleanup_pending = interop_fallback_pending_ || graphics.cleanup_pending;
    result.presentation_suspended = graphics.presentation_suspended;
    result.captured_frames = captured_frames_;
    result.presented_frames = graphics.presented_frames;
    result.dropped_frames = dropped_frames_ + graphics.dropped_frames;
    result.slots_in_flight = graphics.slots_in_flight;
    result.presentations_in_flight = graphics.presentations_in_flight;
    if (graphics.next_deadline) earlier_deadline(result.next_deadline, *graphics.next_deadline);
    result.presentation = graphics.presentation;
    return result;
  }


  WindowsRenderStack::WindowsRenderStack(
    platform::windows::OverlayWindow &overlay,
    const std::uint64_t resource_generation,
    const WindowsBridgePreference bridge_preference,
    diagnostics::PerformanceReport *performance,
    const std::uint64_t observation_instance)
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
