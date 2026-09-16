/// @file
/// @brief Connect process services and concrete adapters, then honor the host's exit decision.
///
/// Construction order establishes the lifetime of borrowed command/wake targets.
/// Runtime sequencing belongs below this composition root; this file only wires
/// services, reports startup failures, and chooses normal or non-unwinding exit.
#include "avb/app/app_command_queue.hpp"
#include "avb/app/application.hpp"
#include "avb/app/config_store.hpp"
#include "avb/desktop/desktop_host.hpp"
#include "avb/diagnostics/performance_report.hpp"
#include "platform/windows/capture_exclusion.hpp"
#include "platform/windows/windows_controller_window.hpp"
#include "platform/windows/controller_font.hpp"
#include "platform/windows/process_dpi_awareness.hpp"
#include "platform/windows/windows_atomic_replace.hpp"
#include "platform/windows/windows_global_hotkey.hpp"
#include "platform/windows/windows_runtime_factory.hpp"
#include "platform/windows/windows_source_catalog.hpp"
#include "platform/windows/windows_source_registry.hpp"
#include "platform/windows/windows_ui_language.hpp"

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
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace {

  /// @brief Process-local observation options, validated before native startup.
  /// The report destination has exclusive-create semantics. Capture preferences
  /// here select this diagnostic run; persisted user preferences keep their owner.
  struct PerformanceOptions {
    std::optional<std::filesystem::path> report;
    avb::diagnostics::PerformanceScope scope{avb::diagnostics::PerformanceScope::Replica};
    avb::render::WindowsBridgePreference bridge{avb::render::WindowsBridgePreference::Automatic};
  };

  [[nodiscard]] std::expected<PerformanceOptions, std::string_view>
  parse_performance_options(const int argc, char *argv[])
  {
    PerformanceOptions options;
    bool scope_seen{}, bridge_seen{};
    for (int index = 1; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (index + 1 == argc) {
        return std::unexpected{"option requires a value"};
      }
      const std::string_view value{argv[++index]};
      if (value.empty() || value.starts_with("--")) {
        return std::unexpected{"option requires a value"};
      }
      if (option == "--performance-report" && !options.report) {
        options.report = std::filesystem::path{value};
      }
      else if (option == "--performance-scope" && !scope_seen) {
        scope_seen = true;
        if (value == "controller") options.scope = avb::diagnostics::PerformanceScope::Controller;
        else if (value != "replica") return std::unexpected{"unknown performance scope"};
      }
      else if (option == "--performance-bridge" && !bridge_seen) {
        bridge_seen = true;
        if (value == "cpu") options.bridge = avb::render::WindowsBridgePreference::ForceCpuBaseline;
        else if (value != "auto") return std::unexpected{"unknown performance bridge"};
      }
      else {
        return std::unexpected{"unknown or duplicate option"};
      }
    }
    if ((scope_seen || bridge_seen) && !options.report) {
      return std::unexpected{"performance scope/bridge requires --performance-report"};
    }
    if (options.report) {
      std::error_code error;
      const auto parent = options.report->has_parent_path()
        ? options.report->parent_path() : std::filesystem::path{"."};
      if (!std::filesystem::is_directory(parent, error) || error) {
        return std::unexpected{"report parent directory must exist"};
      }
      if (std::filesystem::exists(*options.report, error) || error) {
        return std::unexpected{"report path must be new"};
      }
    }
    return options;
  }

  // Static UI capabilities describe behavior; actual bridge support is measured
  // only after a concrete capture/graphics generation has been constructed.
  constexpr avb::core::PlatformCapabilities windows_capabilities{
    .display_capture = true,
    .window_capture = true,
    .pass_through = true,
    .borderless_resize = true,
    .global_hotkey = true,
    .capture_exclusion = true,
    .browser_capture_keep_alive = true,
  };

  /// Print fixed diagnostics/native codes without source titles or pixel data.
  void report_error(std::string_view context,
                    const avb::core::AppError error) noexcept
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

  /// @brief Resolve executable-relative preferences without depending on the caller's CWD.
  ///
  /// Launching through a shortcut or shell must not silently select a different
  /// preferences file. Query the wide executable path and reject truncation before
  /// constructing the sibling path. This only resolves a location: existence,
  /// readability and writing policy belong to the configured persistence service.
  /// Path/projection failures are contained as typed startup errors.
  [[nodiscard]] std::expected<std::filesystem::path,
                              avb::core::AppError>
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
        return std::unexpected{avb::core::AppError{
          avb::core::AppErrorCode::RuntimeConstructionFailure,
          static_cast<std::int64_t>(detail)}};
      }

      auto executable =
        std::filesystem::path{path.data(), path.data() + length};
      return executable.parent_path() / L"avb0795.config";
    } catch (...) {
      return std::unexpected{avb::core::AppError{
        avb::core::AppErrorCode::RuntimeConstructionFailure}};
    }
  }

  /// @brief Construct the object graph in the order required by its borrowed callbacks.
  ///
  /// The command queue precedes the controller; native services that borrow the
  /// controller's wake target are built afterward. Normal reverse destruction
  /// therefore removes those services before their targets. The host drives the
  /// actual session and shutdown workflow; this function only wires dependencies.
  /// An abandon result is handled while every owner is still alive, because a
  /// normal return would destroy resources whose completion has not been proven.
  [[nodiscard]] int run_application(const PerformanceOptions &options)
  {
    using namespace avb;

    // This owner precedes every borrower, including startup-failure unwinding.
    auto performance = options.report
      ? std::make_unique<diagnostics::PerformanceReport>(options.scope) : nullptr;

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
      [](GLFWwindow *window)
        -> std::expected<desktop::ControllerWindowSetup, core::AppError> {
        const auto native_window = glfwGetWin32Window(window);
        auto exclusion = platform::windows::prepare_controller_window(native_window);
        if (!exclusion) {
          return std::unexpected{exclusion.error()};
        }
        auto interaction = platform::windows::WindowsControllerWindow::create(native_window);
        if (!interaction) {
          return std::unexpected{interaction.error()};
        }
        return desktop::ControllerWindowSetup{*exclusion, std::move(*interaction)};
      },
      std::move(*font_data), performance.get());
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
      source_registry, [&host]() noexcept { host.wake(); }, options.bridge, performance.get()};

    auto hotkey_result = platform::windows::WindowsGlobalHotkey::create(
      [&commands, &host](core::HotkeyChord chord) {
        // Keep OS registration active while the focused recorder receives the
        // same confirmed chord. Other events enter the normal recovery queue.
        if (host.try_record_hotkey(chord)) {
          host.wake();
        }
        else if (commands.push(app::RestoreActiveReplicaEditable{})) {
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
      platform::windows::windows_ui_language,
      config_path->parent_path() / L"airelinna.json",
    };

    const auto result = host.run(application);
    // A stalled/uncertain native owner is deliberately still alive. Stack unwind
    // would violate its destruction invariant, so process exit owns final cleanup.
    if (result.abandon_process) {
      std::fputs(
        "avb0795 retained uncertain native ownership; "
        "terminating without stack unwinding.\n",
        stderr);
      std::fflush(nullptr);
      std::_Exit(result.exit_code != 0 ? result.exit_code : 7);
    }
    if (performance) {
      performance->finish(diagnostics::PerformanceReport::Clock::now());
      if (const auto written = performance->write_new(*options.report); !written) {
        std::fprintf(stderr, "performance report write failed (reason=%d)\n",
                     static_cast<int>(written.error()));
        return 8;
      }
    }
    return result.exit_code;
  }

}    // namespace

/// @brief Establish process prerequisites before constructing any product window.
///
/// DPI policy comes first so later window geometry is interpreted consistently.
/// COM and GLFW remain initialized until a normal return has unwound all composed
/// services. Startup failures have fixed diagnostic exit paths; the separate
/// abandon path deliberately never returns here to perform ordinary teardown.
int main(const int argc, char *argv[])
{
  PerformanceOptions options;
  try {
    const auto parsed = parse_performance_options(argc, argv);
    if (!parsed) {
      std::fprintf(stderr, "%.*s\nUsage: avb0795 [--performance-report <new.json> "
                          "[--performance-scope controller|replica] "
                          "[--performance-bridge auto|cpu]]\n",
                   static_cast<int>(parsed.error().size()), parsed.error().data());
      return 1;
    }
    options = *parsed;
  } catch (...) {
    std::fputs("invalid performance report options\n", stderr);
    return 1;
  }
  const auto dpi =
    avb::platform::windows::establish_per_monitor_v2_dpi_awareness();
  if (!dpi) {
    report_error("DPI awareness", dpi.error());
    return 2;
  }

  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (const winrt::hresult_error &error) {
    report_error(
      "COM initialization",
      avb::core::AppError{
        avb::core::AppErrorCode::RuntimeConstructionFailure,
        static_cast<std::int64_t>(error.code().value)});
    return 3;
  } catch (...) {
    report_error(
      "COM initialization",
      avb::core::AppError{
        avb::core::AppErrorCode::RuntimeConstructionFailure});
    return 3;
  }

  glfwSetErrorCallback(glfw_error_callback);
  if (glfwInit() != GLFW_TRUE) {
    report_error(
      "GLFW initialization",
      avb::core::AppError{
        avb::core::AppErrorCode::RuntimeConstructionFailure});
    winrt::uninit_apartment();
    return 4;
  }

  int exit_code{};
  try {
    exit_code = run_application(options);
  } catch (const avb::core::AppError &error) {
    report_error("application startup", error);
    exit_code = 6;
  } catch (...) {
    report_error(
      "application startup",
      avb::core::AppError{
        avb::core::AppErrorCode::RuntimeConstructionFailure});
    exit_code = 6;
  }

  glfwTerminate();
  winrt::uninit_apartment();
  return exit_code;
}
