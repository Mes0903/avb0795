/// @file
/// Windows composition root: initialize process services, connect concrete
/// adapters to portable application ports, and honor the host's exit decision.
#include "airelinna/app/app_command_queue.hpp"
#include "airelinna/app/application.hpp"
#include "airelinna/app/config_store.hpp"
#include "airelinna/desktop/desktop_host.hpp"
#include "platform/windows/capture_exclusion.hpp"
#include "platform/windows/controller_font.hpp"
#include "platform/windows/process_dpi_awareness.hpp"
#include "platform/windows/windows_atomic_replace.hpp"
#include "platform/windows/windows_global_hotkey.hpp"
#include "platform/windows/windows_runtime_factory.hpp"
#include "platform/windows/windows_source_catalog.hpp"
#include "platform/windows/windows_source_registry.hpp"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <winrt/base.h>

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <string_view>
#include <utility>

namespace {

  // Static UI capabilities describe behavior; actual bridge support is measured
  // only after a concrete capture/graphics generation has been constructed.
  constexpr airelinna::core::PlatformCapabilities windows_capabilities{
    .display_capture = true,
    .window_capture = true,
    .pass_through = true,
    .borderless_resize = true,
    .global_hotkey = true,
    .capture_exclusion = true,
  };

  /// Print fixed diagnostics/native codes without source titles or pixel data.
  void report_error(std::string_view context,
                    const airelinna::core::AppError error) noexcept
  {
    const auto label = error.diagnostic_label();
    std::fprintf(stderr, "%.*s: %.*s", static_cast<int>(context.size()),
                 context.data(), static_cast<int>(label.size()), label.data());
    if (error.native_detail) {
      std::fprintf(stderr, " (native=%lld)",
                   static_cast<long long>(*error.native_detail));
    }
    std::fputc('\n', stderr);
  }

  /// Contain the GLFW logging callback within its C-facing no-throw boundary.
  void glfw_error_callback(int code, const char *description) noexcept
  {
    std::fprintf(stderr, "GLFW error %d: %s\n", code,
                 description != nullptr ? description : "unknown");
  }

  /// Resolve config beside the executable, independent of the launch directory.
  /// Path/projection failure becomes a typed startup error rather than an escape.
  [[nodiscard]] std::expected<std::filesystem::path,
                              airelinna::core::AppError>
  executable_config_path() noexcept
  {
    try {
      std::array<wchar_t, 32768> path{};
      SetLastError(ERROR_SUCCESS);
      const auto length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
      if (length == 0 || length >= path.size()) {
        auto detail = GetLastError();
        if (detail == ERROR_SUCCESS) {
          detail = ERROR_INSUFFICIENT_BUFFER;
        }
        return std::unexpected{airelinna::core::AppError{
          airelinna::core::AppErrorCode::RuntimeConstructionFailure,
          static_cast<std::int64_t>(detail)}};
      }

      auto executable =
        std::filesystem::path{path.data(), path.data() + length};
      return executable.parent_path() / L"airelinna.json";
    } catch (...) {
      return std::unexpected{airelinna::core::AppError{
        airelinna::core::AppErrorCode::RuntimeConstructionFailure}};
    }
  }

  /// Construct owners in dependency order and keep every borrowed callback
  /// target alive through host-driven shutdown. No session sequencing lives here.
  [[nodiscard]] int run_application()
  {
    using namespace airelinna;

    // Queue outlives callbacks; the host is constructed before native services
    // whose admitted wake operations borrow it.
    app::AppCommandQueue commands;
    auto font_data = platform::windows::load_controller_font();
    if (!font_data) {
      report_error("controller font (Windows Microsoft JhengHei)", font_data.error());
      return 5;
    }
    auto host_result = desktop::DesktopHost::create(
      [&commands](auto command) {
        return commands.push(std::move(command));
      },
      [](GLFWwindow *window) {
        return platform::windows::prepare_controller_window(
          glfwGetWin32Window(window));
      },
      std::move(*font_data));
    if (!host_result) {
      report_error("controller startup", host_result.error());
      return 5;
    }
    auto host = std::move(*host_result);

    // Reverse stack destruction releases the application/services and registry
    // observations before controller graphics, the GLFW window and font bytes.
    platform::windows::WindowsSourceRegistry source_registry;
    platform::windows::WindowsSourceCatalog source_catalog{source_registry};
    platform::windows::WindowsRuntimeFactory runtime_factory{
      source_registry, [&host]() noexcept { host.wake(); }};

    auto hotkey_result = platform::windows::WindowsGlobalHotkey::create(
      [&commands, &host](auto command) {
        if (commands.push(std::move(command))) {
          host.wake();
        }
      });
    if (!hotkey_result) {
      report_error("hotkey startup", hotkey_result.error());
      return 6;
    }
    auto global_hotkey = std::move(*hotkey_result);

    const auto config_path = executable_config_path();
    if (!config_path) {
      report_error("config path", config_path.error());
      return 6;
    }
    app::ConfigStore config_store{
      platform::windows::windows_atomic_replace};
    app::Application application{
      commands,
      source_catalog,
      runtime_factory,
      *global_hotkey,
      config_store,
      *config_path,
      windows_capabilities,
      host.controller_exclusion(),
    };

    const auto result = host.run(application);
    // A stalled/uncertain native owner is deliberately still alive. Stack unwind
    // would violate its destruction invariant, so process exit owns final cleanup.
    if (result.abandon_process) {
      std::fputs(
        "Airelinna retained uncertain native ownership; "
        "terminating without stack unwinding.\n",
        stderr);
      std::fflush(nullptr);
      std::_Exit(result.exit_code != 0 ? result.exit_code : 7);
    }
    return result.exit_code;
  }

}    // namespace

/// Establish DPI, COM and GLFW before any product window; terminate them only
/// after a normal host return has safely unwound the composed application.
int main()
{
  const auto dpi =
    airelinna::platform::windows::establish_per_monitor_v2_dpi_awareness();
  if (!dpi) {
    report_error("DPI awareness", dpi.error());
    return 2;
  }

  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (const winrt::hresult_error &error) {
    report_error(
      "COM initialization",
      airelinna::core::AppError{
        airelinna::core::AppErrorCode::RuntimeConstructionFailure,
        static_cast<std::int64_t>(error.code().value)});
    return 3;
  } catch (...) {
    report_error(
      "COM initialization",
      airelinna::core::AppError{
        airelinna::core::AppErrorCode::RuntimeConstructionFailure});
    return 3;
  }

  glfwSetErrorCallback(glfw_error_callback);
  if (glfwInit() != GLFW_TRUE) {
    report_error(
      "GLFW initialization",
      airelinna::core::AppError{
        airelinna::core::AppErrorCode::RuntimeConstructionFailure});
    winrt::uninit_apartment();
    return 4;
  }

  int exit_code{};
  try {
    exit_code = run_application();
  } catch (const airelinna::core::AppError &error) {
    report_error("application startup", error);
    exit_code = 6;
  } catch (...) {
    report_error(
      "application startup",
      airelinna::core::AppError{
        airelinna::core::AppErrorCode::RuntimeConstructionFailure});
    exit_code = 6;
  }

  glfwTerminate();
  winrt::uninit_apartment();
  return exit_code;
}
