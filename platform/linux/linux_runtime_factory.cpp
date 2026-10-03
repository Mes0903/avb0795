/// @file
/// @brief Fallible unpublished preparation followed by synchronized no-throw publication.
#include "platform/linux/linux_runtime_factory.hpp"
#include "platform/linux/linux_replica_runtime.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <limits>
#include <utility>
namespace avb::linux_platform {
  namespace {
    /// @brief Schedule exact preparation rejection while the registry retains capture.
    /// Borrows are confined to create. Never destructs or joins native execution.
    struct PreparedRejection final {
      LinuxSourceRegistry &registry;
      const core::SourceId &id;
      PreparedSourceSerial serial;
      bool committed{};
      ~PreparedRejection()
      {
        if (!committed) registry.reject_prepared(id, serial);
      }
    };
    /// @brief Validate complete settings before any native preparation is requested.
    bool valid_initial_settings(const core::ReplicaSettings &settings) noexcept
    {
      return !settings.selecting_region && settings.opacity_percent >= 0 && settings.opacity_percent <= 100 &&
             core::valid_wheel_zoom_percent(settings.wheel_zoom_percent) &&
             (settings.mode == core::OverlayMode::Editable || settings.mode == core::OverlayMode::PassThrough) &&
             (settings.border == core::BorderPreference::Bordered || settings.border == core::BorderPreference::Borderless) &&
             (!settings.region || (settings.region->width && settings.region->height)) &&
             (settings.fps_cap == core::FpsCap::Fps30 || settings.fps_cap == core::FpsCap::Fps60 ||
              settings.fps_cap == core::FpsCap::Fps144 || settings.fps_cap == core::FpsCap::Unlimited);
    }
    /// @brief Preserve actionable admission errors while mapping construction failures.
    core::AppError construction_error(core::AppError error) noexcept
    {
      if (error.code != core::AppErrorCode::InvalidValue && error.code != core::AppErrorCode::UnsupportedGraphics &&
          error.code != core::AppErrorCode::PermissionDenied)
        error.code = core::AppErrorCode::RuntimeConstructionFailure;
      return error;
    }
  }    // namespace
  LinuxRuntimeFactory::LinuxRuntimeFactory(LinuxSourceRegistry &registry, std::function<void()> wake, diagnostics::PerformanceReport *performance) : registry_{registry}, wake_{std::move(wake)}, performance_{performance} {}
  std::expected<std::unique_ptr<app::IReplicaRuntime>, core::AppError>
  LinuxRuntimeFactory::create(const core::SourceDescriptor &source, const core::ReplicaSettings &settings)
  {
    if (!valid_initial_settings(settings)) return std::unexpected(core::AppError{core::AppErrorCode::InvalidValue});
    auto borrowed = registry_.borrow_prepared(source.id);
    if (!borrowed) return std::unexpected(construction_error(borrowed.error()));
    PreparedRejection reject{registry_, source.id, borrowed->serial};
    try {
      // Both supported negotiated formats are normalized to BGRA by the capture
      // pool; the CPU upload ring has that fixed renderer format.
      if (borrowed->format != detail::CpuPixelFormat::Bgra && borrowed->format != detail::CpuPixelFormat::Bgrx)
        throw core::AppError{core::AppErrorCode::RuntimeConstructionFailure};
      // GLFW hints are global: controller hints must not leak into this replica.
      glfwDefaultWindowHints();
      glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
      glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
      glfwWindowHint(GLFW_DECORATED, settings.border == core::BorderPreference::Bordered);
      glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
      ReplicaWindow window{glfwCreateWindow(static_cast<int>(std::min(borrowed->content_extent.width, 960u)),
                                            static_cast<int>(std::min(borrowed->content_extent.height, 540u)), "Replica", nullptr, nullptr)};
      if (!window) throw core::AppError{core::AppErrorCode::RuntimeConstructionFailure};
      auto adapter = X11OverlayAdapter::create(window.get());
      if (!adapter) throw adapter.error();
      if (auto accepted = (*adapter)->apply(settings); !accepted) throw accepted.error();
      // WM visibility is a request. Its visual completion never substitutes for GPU proof.
      glfwShowWindow(window.get());
      if (performance_ && next_observation_instance_ == std::numeric_limits<std::uint64_t>::max())
        throw core::AppError{core::AppErrorCode::RuntimeConstructionFailure};
      const auto observation = performance_ ? next_observation_instance_++ : 0;
      auto renderer = std::make_unique<render::VulkanRenderer>(window.get(), borrowed->allocation_revision,
                                                               detail::replica_framebuffer(window.get()), render::VulkanStartupRequest{}, performance_, observation);
      renderer->set_source_admission(false);
      const auto serial = detail::prepare_cpu_ring(*renderer, borrowed->allocation_extent, borrowed->allocation_revision);
      renderer->set_presentation({settings.region, false, {}});
      auto runtime = std::make_unique<LinuxReplicaRuntime>(LinuxReplicaRuntime::ConstructionToken{}, std::move(window),
                                                           std::move(*adapter), std::move(renderer), settings, borrowed->allocation_revision, serial, wake_, performance_, observation);
      // Runtime allocation and callable copies are complete. The registry binds
      // current serial/revision/first-frame atomically BEFORE moving its live owner.
      auto committed = registry_.commit_prepared(source.id, borrowed->serial, borrowed->allocation_revision);
      if (!committed) return std::unexpected(construction_error(committed.error()));
      runtime->attach_capture(std::move(committed->capture));
      reject.committed = true;
      std::unique_ptr<app::IReplicaRuntime> result{std::move(runtime)};
      return result;
    } catch (const core::AppError &error) {
      return std::unexpected(construction_error(error));
    } catch (...) {
      return std::unexpected(core::AppError{core::AppErrorCode::RuntimeConstructionFailure});
    }
  }
}    // namespace avb::linux_platform
