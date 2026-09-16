/// @file
/// @brief Construction and publication of the complete Windows replica aggregate.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/windows_runtime_factory.hpp"

#include "platform/windows/detail/overlay_transactions.hpp"
#include "platform/windows/features/browser_capture_keep_alive.hpp"
#include "platform/windows/overlay_window.hpp"
#include "platform/windows/wgc_capture_job.hpp"
#include "platform/windows/win32_overlay_adapter.hpp"
#include "platform/windows/windows_capture_source.hpp"
#include "platform/windows/windows_replica_runtime.hpp"
#include "render/windows_render_stack.hpp"

#include <dxgiformat.h>
#include <winerror.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace avb::platform::windows {
  namespace {

    constexpr std::uint64_t initial_resource_generation{1};
    constexpr std::uint32_t capture_pool_buffer_count{5};
    constexpr std::uint32_t maximum_initial_overlay_width{960};
    constexpr std::uint32_t maximum_initial_overlay_height{540};

    [[nodiscard]] constexpr core::AppError
    factory_error(const core::AppError &error) noexcept
    {
      switch (error.code) {
      case core::AppErrorCode::SourceUnavailable:
      case core::AppErrorCode::PermissionDenied:
      case core::AppErrorCode::UnsupportedWindowClass:
      case core::AppErrorCode::UnsupportedGraphics:
        return error;
      default:
        return {.code = core::AppErrorCode::RuntimeConstructionFailure,
                .native_detail = error.native_detail};
      }
    }

    [[nodiscard]] constexpr core::Extent
    initial_overlay_extent(const core::Extent source) noexcept
    {
      return {.width = (std::min) (source.width, maximum_initial_overlay_width),
              .height = (std::min) (source.height, maximum_initial_overlay_height)};
    }

    struct AttachmentCleanupOperations final {
      OverlayWindow *overlay{};
      Win32OverlayAdapter *adapter{};

      [[nodiscard]] bool destroy_with_adapter_alive_required() const noexcept
      {
        return adapter != nullptr && adapter->window_destruction_required();
      }

      [[nodiscard]] bool detach_adapter() noexcept
      {
        return adapter == nullptr || adapter->detach().has_value();
      }

      void destroy_overlay() noexcept
      {
        if (overlay != nullptr) {
          overlay->destroy();
        }
      }

      void complete_adapter_after_overlay_destroyed() noexcept
      {
        if (adapter != nullptr) {
          adapter->complete_after_overlay_destroyed();
        }
      }
    };

  }    // namespace

  WindowsRuntimeFactory::WindowsRuntimeFactory(
    WindowsSourceRegistry &registry, WakeMainLoop wake_main_loop, const render::WindowsBridgePreference bridge_preference,
    diagnostics::PerformanceReport *performance)
      : registry_{registry}, wake_main_loop_{std::move(wake_main_loop)}, bridge_preference_{bridge_preference}, performance_{performance} {}

  std::expected<std::unique_ptr<app::IReplicaRuntime>, core::AppError>
  WindowsRuntimeFactory::create(const core::SourceDescriptor &source,
                                const core::ReplicaSettings &settings)
  {
    try {
      if (settings.selecting_region) {
        return std::unexpected{core::AppError{.code = core::AppErrorCode::InvalidValue}};
      }
      const auto overlay_extent = initial_overlay_extent(source.extent);
      if (overlay_extent.width == 0 || overlay_extent.height == 0) {
        return std::unexpected{core::AppError{
          .code = core::AppErrorCode::RuntimeConstructionFailure}};
      }

      auto lease = registry_.resolve(source.id);
      if (!lease) {
        return std::unexpected{factory_error(lease.error())};
      }
      // Resolve once: every later job borrows this same catalog-published identity.
      auto source_owner =
        WindowsCaptureSource::create(std::move(*lease), wake_main_loop_);
      if (!source_owner) {
        return std::unexpected{factory_error(source_owner.error())};
      }

      // HWND must precede adapter/subclass and Vulkan surface construction.
      auto overlay = OverlayWindow::create(overlay_extent, settings.border);
      if (!overlay) {
        return std::unexpected{factory_error(overlay.error())};
      }
      // Replicas retain the default capturable window policy. A display source
      // can include this overlay; choosing a source window or placing the
      // overlay on another display keeps that feedback out of the source image.
      constexpr auto exclusion = core::CaptureExclusionStatus::NotAttempted;

      auto adapter = Win32OverlayAdapter::create_detached(
        **overlay, settings.mode, settings.border, source.extent);
      if (!adapter) {
        return std::unexpected{factory_error(adapter.error())};
      }

      std::unique_ptr<WindowsReplicaRuntime> runtime;
      AttachmentCleanupOperations cleanup{.overlay = overlay->get(),
                                          .adapter = adapter->get()};
      // Cleanup follows the partial pair into runtime without adding a long-lived service.
      detail::FactoryOverlayAttachmentGuard guard{cleanup};
      const auto attached = (*adapter)->attach();
      if (!attached) {
        return std::unexpected{factory_error(attached.error())};
      }
      // Guard ownership follows subclass installation before any later fallible step.
      guard.arm();

      // Establish persisted whole-window alpha before capture can start or be published.
      const auto presented = (*adapter)->set_presentation(settings.mode,
                                                           settings.opacity_percent);
      if (!presented) {
        return std::unexpected{factory_error(presented.error())};
      }

      if (performance_ && next_observation_instance_ == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected{core::AppError{core::AppErrorCode::RuntimeConstructionFailure}};
      }
      const auto observation_instance = performance_ ? next_observation_instance_++ : 0;
      auto render_stack = std::make_unique<render::WindowsRenderStack>(
        **overlay, initial_resource_generation, bridge_preference_, performance_, observation_instance);
      render_stack->set_performance_fps(settings.fps_cap);
      render_stack->set_presentation({settings.region, false, {}});
      auto capture_job = WgcCaptureJob::create_inactive(
        **source_owner, render_stack->capture_device(),
        initial_resource_generation, capture_pool_buffer_count,
        wake_main_loop_);
      if (!capture_job) {
        return std::unexpected{factory_error(capture_job.error())};
      }
      // Descriptor extent may be stale; render slots must match the actual pool allocation.
      const auto allocation = (*capture_job)->initial_allocation();
      const auto slots = render_stack->rebuild_capture_slots(
        initial_resource_generation, allocation, DXGI_FORMAT_B8G8R8A8_UNORM);
      if (!slots) {
        return std::unexpected{factory_error(slots.error())};
      }
      // Actual presented content establishes the eventual crop/resize constraint.
      (*adapter)->update_source_extent({});

      std::unique_ptr<features::BrowserCaptureKeepAlive> browser_keep_alive;
      if (source.kind == core::SourceKind::Window) {
        browser_keep_alive =
          std::make_unique<features::BrowserCaptureKeepAlive>();
        if (settings.extensions.browser_capture_keep_alive) {
          const auto prepared = browser_keep_alive->prepare_hidden();
          if (!prepared) {
            return std::unexpected{factory_error(prepared.error())};
          }
        }
      }

      runtime = std::make_unique<WindowsReplicaRuntime>(
        WindowsReplicaRuntime::ConstructionToken{}, std::move(*source_owner),
        std::move(*overlay), std::move(*adapter), std::move(render_stack),
        std::move(*capture_job), std::move(browser_keep_alive), settings,
        allocation, exclusion, bridge_preference_, capture_pool_buffer_count,
        wake_main_loop_, performance_, observation_instance);
      // The complete aggregate exists before the last fallible StartCapture operation.
      const auto activated = runtime->activate_for_publication();
      if (!activated) {
        runtime->cleanup_unpublished_activation_failure();
        return std::unexpected{factory_error(activated.error())};
      }

      // Only no-throw ownership transfer remains after native publication succeeds.
      std::unique_ptr<app::IReplicaRuntime> published{runtime.release()};
      guard.disarm();
      return published;
    } catch (const core::AppError &error) {
      return std::unexpected{factory_error(error)};
    } catch (const std::bad_alloc &) {
      return std::unexpected{
        core::AppError{.code = core::AppErrorCode::RuntimeConstructionFailure,
                       .native_detail = E_OUTOFMEMORY}};
    } catch (...) {
      return std::unexpected{
        core::AppError{.code = core::AppErrorCode::RuntimeConstructionFailure,
                       .native_detail = E_FAIL}};
    }
  }

}    // namespace avb::platform::windows
