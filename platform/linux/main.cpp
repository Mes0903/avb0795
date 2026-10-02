/// @file
/// @brief Compose the shared controller with Linux services and honor its exit decision.
///
/// The stack establishes borrowed command/window lifetimes. Native operations
/// remain in the concrete adapters; this entry only wires values/services and
/// reports fixed startup failures. Capture is unavailable at this milestone.
#include "avb/app/app_command_queue.hpp"
#include "avb/app/application.hpp"
#include "avb/app/config_store.hpp"
#include "avb/desktop/desktop_host.hpp"
#include "platform/linux/controller_font.hpp"
#include "platform/linux/linux_atomic_replace.hpp"
#include "platform/linux/linux_disabled_hotkey.hpp"
#include "platform/linux/linux_preferences.hpp"
#include "platform/linux/linux_ui_language.hpp"
#include "platform/linux/linux_unavailable_services.hpp"
#include "platform/linux/x11_controller_window.hpp"
#include "platform/linux/x11_overlay_adapter.hpp"

#include <GLFW/glfw3.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <string_view>
#include <utility>

namespace {
  int glfw_error_code{};

  /// @brief Preserve only numeric startup error detail across the C callback boundary.
  /// GLFW startup and this callback run on the main thread. Fixed contexts are
  /// emitted by the caller so native descriptions cannot expose environment data.
  void glfw_error_callback(const int code, const char *) noexcept
  {
    glfw_error_code = code;
  }

  /// @brief Emit a fixed typed context and optional native code without allocating.
  /// Called synchronously on the main thread; no source titles or capture data
  /// enter diagnostics, and logging failure never changes destruction authority.
  void report_error(const std::string_view context, const avb::core::AppError error) noexcept
  {
    const auto label = error.diagnostic_label();
    std::fprintf(stderr, "%.*s: %.*s", static_cast<int>(context.size()), context.data(),
                 static_cast<int>(label.size()), label.data());
    if (error.native_detail) {
      std::fprintf(stderr, " (native=%lld)", static_cast<long long>(*error.native_detail));
    }
    std::fputc('\n', stderr);
  }

  /// @brief Construct the synchronous controller graph in borrow-lifetime order.
  /// GLFW is initialized on this main thread. The queue outlives the host's
  /// retained command sink; the preference lock outlives all persistence calls.
  /// Source/hotkey services create no workers or asynchronous wake borrowers.
  /// Startup failure unwinds only never-submitted host resources. After run,
  /// ordinary destruction is permitted solely by a non-abandon host result;
  /// uncertain submitted owners remain in scope until the non-unwinding exit.
  [[nodiscard]] int run_application()
  {
    using namespace avb;
    app::AppCommandQueue commands;
    auto font = linux_platform::load_controller_font();
    if (!font) {
      report_error("controller font (Linux Noto CJK)", font.error());
      return 5;
    }
    core::PlatformCapabilities capabilities{.window_opacity = false};
    auto host_result = desktop::DesktopHost::create(
      [&commands](auto command) { return commands.push(std::move(command)); },
      [&capabilities](GLFWwindow *window)
        -> std::expected<desktop::ControllerWindowSetup, core::AppError> {
        {
          // The host's hidden, unsubmitted window supplies real protocol facts.
          // The probe restores opacity and detaches its callback chain BEFORE
          // the lasting controller interaction and subsequent UI hooks attach.
          auto probe = linux_platform::X11OverlayAdapter::create(window);
          if (!probe) return std::unexpected{probe.error()};
          capabilities = (*probe)->capabilities();
        }
        auto interaction = linux_platform::X11ControllerWindow::create(window);
        if (!interaction) return std::unexpected{interaction.error()};
        return desktop::ControllerWindowSetup{
          core::CaptureExclusionStatus::NotAttempted, std::move(*interaction)};
      },
      std::move(*font), "Linux");
    if (!host_result) {
      // UnsupportedGraphics also covers loader/queues/formats. Describe the
      // required floor without guessing which prerequisite this device lacks.
      report_error(host_result.error().code == core::AppErrorCode::UnsupportedGraphics
                     ? "controller startup (requires Vulkan 1.3 and KHR/EXT swapchain-maintenance present fences)"
                     : "controller startup",
                   host_result.error());
      return 5;
    }
    auto host = std::move(*host_result);

    auto preferences = linux_platform::LinuxPreferences::create();
    if (!preferences) {
      report_error("preferences writer lock/startup", preferences.error());
      return 6;
    }
    linux_platform::LinuxUnavailableSourceCatalog source_catalog;
    linux_platform::LinuxUnavailableRuntimeFactory runtime_factory;
    linux_platform::LinuxDisabledHotkey hotkey;
    app::ConfigStore store{linux_platform::linux_atomic_replace};
    // Window facts are observations, while these explicit intermediate policies
    // prohibit capture/authorization even if the stored preferences request them.
    capabilities.display_capture = false;
    capabilities.window_capture = false;
    capabilities.global_hotkey = false;
    capabilities.capture_exclusion = false;
    capabilities.browser_capture_keep_alive = false;
    capabilities.source_selection_method = core::SourceSelectionMethod::SystemPicker;
    app::Application application{
      commands, source_catalog, runtime_factory, hotkey, store, preferences->path(),
      capabilities, host.controller_exclusion(), linux_platform::linux_ui_language};
    const auto result = host.run(application);
    if (result.abandon_process) {
      // Returning would release owners whose completion has not been proven.
      // This branch must remain inside every owner scope, including the lock.
      std::fputs("avb0795 retained uncertain native ownership; terminating without stack unwinding.\n", stderr);
      std::fflush(nullptr);
      std::_Exit(result.exit_code != 0 ? result.exit_code : 7);
    }
    return result.exit_code;
  }
}    // namespace

/// @brief Set process file policy and require X11 before any product service exists.
/// Keep umask 0077 for all persistence, including future retry saves. GLFW remains
/// initialized until normal reverse destruction ends; an abandon result never
/// returns here. Missing X11/XWayland is fatal without another backend attempt.
/// Startup exceptions are contained only for this synchronous, capture-free graph.
int main()
{
  umask(0077);
  glfwSetErrorCallback(glfw_error_callback);
  glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
  if (glfwInit() != GLFW_TRUE) {
    avb::core::AppError error{avb::core::AppErrorCode::RuntimeConstructionFailure};
    if (glfw_error_code != 0) error.native_detail = glfw_error_code;
    report_error("GLFW X11 initialization", error);
    return 4;
  }
  int exit_code{};
  try {
    exit_code = run_application();
  } catch (const avb::core::AppError &error) {
    report_error("application startup", error);
    exit_code = 6;
  } catch (...) {
    report_error("application startup",
                 avb::core::AppError{avb::core::AppErrorCode::RuntimeConstructionFailure});
    exit_code = 6;
  }
  glfwTerminate();
  return exit_code;
}
