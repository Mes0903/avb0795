/// @file
/// @brief Fallible inactive preparation followed by an exclusive no-throw publication tail.
#include "platform/linux/linux_runtime_factory.hpp"
#include "platform/linux/linux_replica_runtime.hpp"
#include "platform/linux/x11_controller_window.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>
namespace avb::linux_platform {
  namespace detail {
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
  }
  namespace {
    // Publication converts only owning pointers and scalar errors. Keep its
    // exception guarantee checked if either value type changes in the future.
    static_assert(std::is_nothrow_constructible_v<std::unique_ptr<app::IReplicaRuntime>, std::unique_ptr<LinuxReplicaRuntime> &&>);
    static_assert(std::is_nothrow_constructible_v<std::expected<std::unique_ptr<app::IReplicaRuntime>, core::AppError>, std::unique_ptr<app::IReplicaRuntime> &&>);
    /// @brief Preserve actionable admission errors while mapping unknown construction failures.
    core::AppError construction_error(core::AppError error) noexcept
    {
      if (error.code != core::AppErrorCode::InvalidValue && error.code != core::AppErrorCode::SourceUnavailable &&
          error.code != core::AppErrorCode::UnsupportedGraphics && error.code != core::AppErrorCode::PermissionDenied)
        error.code = core::AppErrorCode::RuntimeConstructionFailure;
      return error;
    }
  }
  LinuxRuntimeFactory::LinuxRuntimeFactory(X11SourceCatalog &catalog, std::function<void()> wake, diagnostics::PerformanceReport *performance)
      : catalog_{catalog}, wake_{std::move(wake)}, performance_{performance} {}
  std::expected<std::unique_ptr<app::IReplicaRuntime>, core::AppError>
  LinuxRuntimeFactory::create(const core::SourceDescriptor &source, const core::ReplicaSettings &settings)
  {
    if (!detail::valid_initial_settings(settings)) return std::unexpected(core::AppError{core::AppErrorCode::InvalidValue});
    std::unique_ptr<LinuxReplicaRuntime> runtime;
    try {
      auto target = catalog_.resolve(source.id);
      if (!target) return std::unexpected(construction_error(target.error()));
      // Copy the original observation into a private checked watch. Keep this
      // original token to certify the handoff after every fallible owner exists.
      auto capture = X11Capture::create(*target, wake_);
      if (!capture) return std::unexpected(construction_error(capture.error()));
      const auto format = (*capture)->initial_format();
      const auto revision = (*capture)->initial_revision();
      if (!revision || !format.allocation.width || !format.allocation.height ||
          (format.pixels != detail::CpuPixelFormat::Bgra && format.pixels != detail::CpuPixelFormat::Bgrx))
        throw core::AppError{core::AppErrorCode::UnsupportedGraphics};
      // Current capture allocation, never cached descriptor extent, authorizes
      // window/ring sizing. Hidden construction prevents premature selection.
      glfwDefaultWindowHints();
      glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
      glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
      glfwWindowHint(GLFW_DECORATED, settings.border == core::BorderPreference::Bordered);
      glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
      ReplicaWindow window{glfwCreateWindow(static_cast<int>(std::min(format.allocation.width, 960u)),
                                            static_cast<int>(std::min(format.allocation.height, 540u)), "Replica", nullptr, nullptr)};
      if (!window) throw core::AppError{core::AppErrorCode::RuntimeConstructionFailure};
      auto adapter = X11OverlayAdapter::create(window.get());
      if (!adapter) throw adapter.error();
      if (auto accepted = (*adapter)->apply(settings); !accepted) throw accepted.error();
      if (next_observation_instance_ == std::numeric_limits<std::uint64_t>::max())
        throw core::AppError{core::AppErrorCode::RuntimeConstructionFailure};
      // Failed preparations consume identity rather than reusing a capture instance.
      const auto observation = next_observation_instance_++;
      auto renderer = std::make_unique<render::VulkanRenderer>(window.get(), revision,
                                                               detail::replica_framebuffer(window.get()), render::VulkanStartupRequest{}, performance_, observation);
      renderer->set_source_admission(false);
      const auto serial = detail::prepare_cpu_ring(*renderer, format.allocation, revision);
      renderer->set_presentation({settings.region, false, {}});
      if (auto acknowledged = (*capture)->acknowledge_allocation_revision(revision); !acknowledged)
        throw acknowledged.error();
      runtime = std::make_unique<LinuxReplicaRuntime>(LinuxReplicaRuntime::ConstructionToken{}, std::move(window),
                                                      std::move(*adapter), std::move(renderer), std::move(*capture), settings,
                                                      revision, serial, wake_, performance_, observation);
      // Registration remains fallible while capture is inactive. Exclusion names
      // this exact owned drawable and follows Destroy, including failed creation.
      auto drawable = owned_x11_drawable(runtime->window_.get());
      if (!drawable) throw drawable.error();
      if (auto registered = catalog_.register_owned_window(*drawable); !registered) throw registered.error();
      auto current = catalog_.resolve(source.id);
      if (!current) throw current.error();
      if (current->generation() != target->generation() || !target->valid())
        throw core::AppError{core::AppErrorCode::SourceUnavailable};
      // The second lookup certifies only lifetime. The original capture watch and
      // target remain authoritative; its worker observes later geometry changes.
    } catch (const core::AppError &error) {
      return std::unexpected(construction_error(error));
    } catch (...) {
      return std::unexpected(core::AppError{core::AppErrorCode::RuntimeConstructionFailure});
    }
    // start failure has no live worker and permits inactive unwind. Successful
    // activation is outside construction catches: no fallible work can release
    // its owner afterward, even if the worker fails before the first main tick.
    if (auto activated = runtime->activate_capture(); !activated)
      return std::unexpected(construction_error(activated.error()));
    glfwShowWindow(runtime->window_.get());
    std::unique_ptr<app::IReplicaRuntime> result{std::move(runtime)};
    return result;
  }
}    // namespace avb::linux_platform
