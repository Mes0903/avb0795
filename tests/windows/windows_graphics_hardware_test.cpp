#include "platform/windows/overlay_window.hpp"
#include "platform/windows/wgc_capture_job.hpp"
#include "platform/windows/windows_capture_source.hpp"
#include "platform/windows/windows_frame_bridge.hpp"
#include "platform/windows/windows_replica_runtime.hpp"
#include "platform/windows/windows_runtime_factory.hpp"
#include "platform/windows/windows_source_catalog.hpp"
#include "platform/windows/windows_source_registry.hpp"
#include "platform/windows/windows_render_stack.hpp"
#include "render/vulkan_renderer.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <GLFW/glfw3.h>

#include <windows.h>
#include <windows.graphics.capture.interop.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

  using namespace std::chrono_literals;
  using avb::core::AppError;
  using avb::core::AppErrorCode;
  using avb::core::BorderPreference;
  using avb::core::Extent;
  using avb::core::SourceDescriptor;
  using avb::core::SourceKind;
  using avb::platform::windows::OverlayWindow;
  using avb::platform::windows::WgcCaptureJob;
  using avb::platform::windows::WindowsCaptureSource;
  using avb::platform::windows::WindowsReplicaRuntime;
  using avb::platform::windows::WindowsRuntimeFactory;
  using avb::platform::windows::WindowsSourceCatalog;
  using avb::platform::windows::WindowsSourceRegistry;
  using avb::render::WindowsBridgePreference;
  using avb::render::WindowsRenderStack;

  constexpr int hardware_skip_exit_code = 77;
  constexpr std::wstring_view consent_environment =
    L"AVB0795_WGC_HARDWARE_CONSENT";
  constexpr std::string_view child_mode = "--avb0795-graphics-source-window";
  constexpr std::wstring_view child_window_class =
    L"avb0795GraphicsHardwareSourceWindow";
  constexpr UINT child_repaint_message = WM_APP + 1;
  constexpr std::uint64_t resource_generation = 1;
  constexpr std::uint32_t pool_buffer_count = 5;
  constexpr auto presentation_timeout = 10s;
  constexpr auto stop_timeout = 10s;
  constexpr auto maximum_wait_slice = 16ms;
  constexpr auto maximum_internal_poll = 50ms;
  constexpr auto source_ready_timeout = 5s;
  constexpr auto child_close_timeout = 5s;

  static_assert(std::is_final_v<WindowsRenderStack>);
  static_assert(!std::is_copy_constructible_v<WindowsRenderStack>);
  static_assert(!std::is_copy_assignable_v<WindowsRenderStack>);
  static_assert(std::is_final_v<WindowsReplicaRuntime>);
  static_assert(std::is_final_v<WindowsRuntimeFactory>);

  [[nodiscard]] std::wstring widen_ascii(const std::string_view value)
  {
    return {value.begin(), value.end()};
  }

  [[nodiscard]] std::string narrow_ascii(const std::wstring_view value)
  {
    return {value.begin(), value.end()};
  }

  LRESULT CALLBACK source_window_proc(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) noexcept
  {
    static bool alternate_fill{};
    switch (message) {
    case child_repaint_message:
      alternate_fill = !alternate_fill;
      InvalidateRect(window, nullptr, TRUE);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      const auto device = BeginPaint(window, &paint);
      FillRect(device, &paint.rcPaint,
               GetSysColorBrush(alternate_fill ? COLOR_HIGHLIGHT : COLOR_WINDOW));
      EndPaint(window, &paint);
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
    }
  }

  class UniqueHandle final {
  public:
    UniqueHandle() = default;
    explicit UniqueHandle(const HANDLE value) noexcept : value_{value} {}
    ~UniqueHandle() noexcept { static_cast<void>(reset()); }

    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&other) noexcept
        : value_{std::exchange(other.value_, nullptr)} {}
    UniqueHandle &operator=(UniqueHandle &&other) noexcept
    {
      if (this != &other) {
        static_cast<void>(reset());
        value_ = std::exchange(other.value_, nullptr);
      }
      return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
      return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] bool reset(const HANDLE replacement = nullptr) noexcept
    {
      const auto previous = std::exchange(value_, replacement);
      return previous == nullptr || previous == INVALID_HANDLE_VALUE ||
             CloseHandle(previous) != FALSE;
    }

  private:
    HANDLE value_{};
  };

  [[noreturn]] void fail_unproven_child_cleanup() noexcept
  {
    static_cast<void>(std::fputs(
      "ERROR: controlled graphics source child cleanup was not proven\n",
      stderr));
    static_cast<void>(std::fflush(stderr));
    std::_Exit(EXIT_FAILURE);
  }

  int run_source_window_child(const std::wstring &ready_event_name,
                              const std::wstring &title) noexcept
  {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = source_window_proc;
    window_class.hInstance = instance;
    window_class.hbrBackground =
      reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    window_class.lpszClassName = child_window_class.data();
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return EXIT_FAILURE;
    }

    const auto window =
      CreateWindowExW(WS_EX_NOACTIVATE, child_window_class.data(), title.c_str(),
                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640,
                      480, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
      return EXIT_FAILURE;
    }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);

    UniqueHandle ready_event{
      OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_event_name.c_str())};
    if (!ready_event || SetEvent(ready_event.get()) == FALSE ||
        !ready_event.reset()) {
      DestroyWindow(window);
      return EXIT_FAILURE;
    }

    MSG message{};
    int result{};
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    return result == -1 ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  class SourceWindowProcess final {
  public:
    SourceWindowProcess() = default;
    ~SourceWindowProcess() noexcept { static_cast<void>(close()); }

    SourceWindowProcess(const SourceWindowProcess &) = delete;
    SourceWindowProcess &operator=(const SourceWindowProcess &) = delete;
    SourceWindowProcess(SourceWindowProcess &&other) noexcept
        : job_{std::move(other.job_)}, process_{std::move(other.process_)}, process_id_{std::exchange(other.process_id_, 0)}, title_{std::move(other.title_)}, cleanup_succeeded_{other.cleanup_succeeded_} {}
    SourceWindowProcess &operator=(SourceWindowProcess &&other) noexcept
    {
      if (this != &other) {
        cleanup_succeeded_ = close();
        job_ = std::move(other.job_);
        process_ = std::move(other.process_);
        process_id_ = std::exchange(other.process_id_, 0);
        title_ = std::move(other.title_);
        cleanup_succeeded_ = cleanup_succeeded_ && other.cleanup_succeeded_;
      }
      return *this;
    }

    [[nodiscard]] static std::optional<SourceWindowProcess>
    create(const std::wstring &title)
    {
      static std::atomic_uint64_t serial{};
      const auto suffix =
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(serial.fetch_add(1, std::memory_order_relaxed));
      const auto event_name = L"Local\\avb0795GraphicsReady-" + suffix;
      UniqueHandle ready_event{
        CreateEventW(nullptr, TRUE, FALSE, event_name.c_str())};
      if (!ready_event) {
        return std::nullopt;
      }

      std::array<wchar_t, MAX_PATH> executable{};
      const auto length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
      if (length == 0 || length == executable.size()) {
        return std::nullopt;
      }

      UniqueHandle job{CreateJobObjectW(nullptr, nullptr)};
      if (!job) {
        return std::nullopt;
      }
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                  &limits, sizeof(limits)) == FALSE) {
        return std::nullopt;
      }

      std::wstring command_line = L"\"" + std::wstring{executable.data()} +
                                  L"\" " + widen_ascii(child_mode) + L" " +
                                  event_name + L" " + title;
      STARTUPINFOW startup{};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process_info{};
      if (CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                         CREATE_SUSPENDED, nullptr, nullptr, &startup,
                         &process_info) == FALSE) {
        return std::nullopt;
      }
      UniqueHandle process{process_info.hProcess};
      UniqueHandle thread{process_info.hThread};
      const auto terminate_via_job = [&]() noexcept {
        if (!job.reset() ||
            WaitForSingleObject(process.get(),
                                static_cast<DWORD>(child_close_timeout.count() *
                                                   1000)) != WAIT_OBJECT_0) {
          fail_unproven_child_cleanup();
        }
      };

      if (AssignProcessToJobObject(job.get(), process.get()) == FALSE) {
        if (TerminateProcess(process.get(), EXIT_FAILURE) == FALSE ||
            WaitForSingleObject(process.get(),
                                static_cast<DWORD>(child_close_timeout.count() *
                                                   1000)) != WAIT_OBJECT_0) {
          fail_unproven_child_cleanup();
        }
        return std::nullopt;
      }
      if (ResumeThread(thread.get()) == (std::numeric_limits<DWORD>::max)()) {
        terminate_via_job();
        return std::nullopt;
      }
      if (WaitForSingleObject(ready_event.get(),
                              static_cast<DWORD>(source_ready_timeout.count() *
                                                 1000)) != WAIT_OBJECT_0) {
        terminate_via_job();
        return std::nullopt;
      }

      SourceWindowProcess result;
      result.job_ = std::move(job);
      result.process_ = std::move(process);
      result.process_id_ = process_info.dwProcessId;
      result.title_ = title;
      result.cleanup_succeeded_ = thread.reset() && ready_event.reset();
      return result;
    }

    [[nodiscard]] const std::wstring &title() const noexcept { return title_; }

    [[nodiscard]] HWND window() const noexcept
    {
      const auto handle = FindWindowW(nullptr, title_.c_str());
      DWORD owner{};
      if (handle != nullptr) {
        static_cast<void>(GetWindowThreadProcessId(handle, &owner));
      }
      return owner == process_id_ ? handle : nullptr;
    }

    [[nodiscard]] bool resize_client(const Extent extent) const noexcept
    {
      const auto handle = window();
      if (handle == nullptr || extent.width == 0 || extent.height == 0 ||
          extent.width >
            static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) ||
          extent.height >
            static_cast<std::uint32_t>((std::numeric_limits<int>::max)())) {
        return false;
      }
      RECT outer{0, 0, static_cast<LONG>(extent.width),
                 static_cast<LONG>(extent.height)};
      const auto style = static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_STYLE));
      const auto extended_style =
        static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_EXSTYLE));
      if (AdjustWindowRectEx(&outer, style, FALSE, extended_style) == FALSE) {
        return false;
      }
      return SetWindowPos(handle, nullptr, 0, 0, outer.right - outer.left,
                          outer.bottom - outer.top,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
    }

    [[nodiscard]] bool request_repaint() const noexcept
    {
      const auto handle = window();
      return handle != nullptr &&
             PostMessageW(handle, child_repaint_message, 0, 0) != FALSE;
    }

    [[nodiscard]] bool close() noexcept
    {
      if (!process_) {
        return cleanup_succeeded_;
      }
      auto succeeded = cleanup_succeeded_;
      if (WaitForSingleObject(process_.get(), 0) != WAIT_OBJECT_0) {
        const auto handle = window();
        if (handle == nullptr || PostMessageW(handle, WM_CLOSE, 0, 0) == FALSE) {
          succeeded = false;
        }
      }
      auto waited = WaitForSingleObject(
        process_.get(), static_cast<DWORD>(child_close_timeout.count() * 1000));
      if (waited != WAIT_OBJECT_0) {
        if (!job_.reset()) {
          fail_unproven_child_cleanup();
        }
        waited = WaitForSingleObject(
          process_.get(),
          static_cast<DWORD>(child_close_timeout.count() * 1000));
      }
      if (waited != WAIT_OBJECT_0) {
        fail_unproven_child_cleanup();
      }
      cleanup_succeeded_ = process_.reset() && job_.reset() && succeeded;
      process_id_ = 0;
      return cleanup_succeeded_;
    }

  private:
    UniqueHandle job_;
    UniqueHandle process_;
    DWORD process_id_{};
    std::wstring title_;
    bool cleanup_succeeded_{true};
  };

  [[nodiscard]] bool is_discovery_invocation(const int argc,
                                             char *const argv[]) noexcept
  {
    for (int index = 1; index < argc; ++index) {
      const std::string_view argument{argv[index]};
      if (argument == "--list-tests" || argument == "--list-test-names-only" ||
          argument == "--list-tags" || argument == "--list-reporters" ||
          argument == "--list-listeners") {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool hardware_consent_present() noexcept
  {
    std::array<wchar_t, 2> value{};
    return GetEnvironmentVariableW(consent_environment.data(), value.data(),
                                   static_cast<DWORD>(value.size())) == 1 &&
           value[0] == L'1';
  }

  [[nodiscard]] bool interactive_desktop_available() noexcept
  {
    const auto desktop = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (desktop == nullptr) {
      return false;
    }
    return CloseDesktop(desktop) != FALSE;
  }

  [[nodiscard]] std::optional<SourceDescriptor>
  find_controlled_source(const std::vector<SourceDescriptor> &sources,
                         const std::string_view label)
  {
    const auto source = std::ranges::find_if(
      sources, [label](const SourceDescriptor &candidate) noexcept {
        return candidate.kind == SourceKind::Window &&
               candidate.display_label == label;
      });
    return source == sources.end() ? std::nullopt
                                   : std::optional<SourceDescriptor>{*source};
  }

  [[nodiscard]] HWND process_owned_overlay() noexcept
  {
    HWND current{};
    while ((current = FindWindowExW(nullptr, current, nullptr,
                                    L"avb0795 Replica")) != nullptr) {
      DWORD process_id{};
      static_cast<void>(GetWindowThreadProcessId(current, &process_id));
      if (process_id == GetCurrentProcessId()) {
        return current;
      }
    }
    return nullptr;
  }

  template <class Event>
  [[nodiscard]] std::size_t
  event_count(const avb::app::RuntimeTickResult &tick) noexcept
  {
    return static_cast<std::size_t>(
      std::ranges::count_if(tick.events, [](const auto &event) noexcept {
        return event && std::holds_alternative<Event>(*event);
      }));
  }

  [[nodiscard]] std::size_t
  occupied_event_count(const avb::app::RuntimeTickResult &tick) noexcept
  {
    return static_cast<std::size_t>(
      std::ranges::count_if(tick.events, [](const auto &event) noexcept {
        return event.has_value();
      }));
  }

  [[nodiscard]] double
  wait_seconds(std::optional<std::chrono::steady_clock::time_point> deadline,
               std::chrono::steady_clock::time_point now) noexcept;

  [[nodiscard]] bool stop_runtime(avb::app::IReplicaRuntime &runtime,
                                  bool &stopped_event_was_lone) noexcept
  {
    runtime.request_stop();
    const auto deadline = std::chrono::steady_clock::now() + stop_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto now = std::chrono::steady_clock::now();
      const auto tick = runtime.tick(now);
      if (event_count<avb::app::Stopped>(tick) != 0) {
        stopped_event_was_lone = occupied_event_count(tick) == 1;
        return stopped_event_was_lone;
      }
      if (!tick.next_wake || *tick.next_wake <= now ||
          *tick.next_wake > now + maximum_internal_poll) {
        return false;
      }
      glfwWaitEventsTimeout(wait_seconds(tick.next_wake, now));
    }
    return false;
  }

  [[nodiscard]] double wait_seconds(
    const std::optional<std::chrono::steady_clock::time_point> deadline,
    const std::chrono::steady_clock::time_point now) noexcept
  {
    const auto wait =
      deadline
        ? std::clamp(*deadline - now, 0ns,
                     std::chrono::duration_cast<std::chrono::nanoseconds>(
                       maximum_wait_slice))
        : maximum_wait_slice;
    return std::chrono::duration<double>{wait}.count();
  }

  [[noreturn]] void fail_without_unwind(const char *const diagnostic) noexcept
  {
    static_cast<void>(std::fputs(diagnostic, stderr));
    static_cast<void>(std::fputc('\n', stderr));
    static_cast<void>(std::fflush(stderr));
    std::_Exit(EXIT_FAILURE);
  }

  class GlfwLifetime final {
  public:
    GlfwLifetime()
    {
      if (glfwInit() != GLFW_TRUE) {
        throw AppError{.code = AppErrorCode::RuntimeConstructionFailure};
      }
    }

    ~GlfwLifetime() noexcept { glfwTerminate(); }

    GlfwLifetime(const GlfwLifetime &) = delete;
    GlfwLifetime &operator=(const GlfwLifetime &) = delete;
  };

  class ApartmentLifetime final {
  public:
    ApartmentLifetime()
    {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    ~ApartmentLifetime() noexcept { winrt::uninit_apartment(); }

    ApartmentLifetime(const ApartmentLifetime &) = delete;
    ApartmentLifetime &operator=(const ApartmentLifetime &) = delete;
  };

  struct GraphicsFixture final {
    WindowsSourceRegistry registry;
    WindowsSourceCatalog catalog{registry};
    SourceDescriptor descriptor;
    std::unique_ptr<WindowsCaptureSource> source;
    std::unique_ptr<OverlayWindow> overlay;
    std::unique_ptr<WindowsRenderStack> render_stack;
    std::unique_ptr<WgcCaptureJob> job;
  };

  [[nodiscard]] std::unique_ptr<GraphicsFixture> create_fixture(const std::string_view source_label)
  {
    auto fixture = std::make_unique<GraphicsFixture>();
    auto sources = fixture->catalog.enumerate();
    if (!sources) {
      throw sources.error();
    }
    const auto selected = find_controlled_source(*sources, source_label);
    if (!selected) {
      throw AppError{.code = AppErrorCode::SourceUnavailable};
    }
    fixture->descriptor = *selected;

    auto lease = fixture->registry.resolve(fixture->descriptor.id);
    if (!lease) {
      throw lease.error();
    }
    auto source = WindowsCaptureSource::create(
      std::move(*lease), []() noexcept { glfwPostEmptyEvent(); });
    if (!source) {
      throw source.error();
    }
    fixture->source = std::move(*source);

    const Extent initial_overlay{
      .width = (std::min) (fixture->descriptor.extent.width, 960U),
      .height = (std::min) (fixture->descriptor.extent.height, 540U)};
    auto overlay =
      OverlayWindow::create(initial_overlay, BorderPreference::Bordered);
    if (!overlay) {
      throw overlay.error();
    }
    fixture->overlay = std::move(*overlay);
    fixture->render_stack = std::make_unique<WindowsRenderStack>(
      *fixture->overlay, resource_generation);

    auto job = WgcCaptureJob::create_inactive(
      *fixture->source, fixture->render_stack->capture_device(),
      resource_generation, pool_buffer_count,
      []() noexcept { glfwPostEmptyEvent(); });
    if (!job) {
      throw job.error();
    }
    fixture->job = std::move(*job);
    const auto slots = fixture->render_stack->rebuild_capture_slots(
      resource_generation, fixture->job->initial_allocation(),
      DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!slots) {
      throw slots.error();
    }
    const auto started = fixture->job->start_for_publication();
    if (!started) {
      throw started.error();
    }
    fixture->source->activate_runtime_wake();
    return fixture;
  }

  [[nodiscard]] bool stop_fixture(GraphicsFixture &fixture) noexcept
  {
    fixture.render_stack->begin_drain();
    fixture.job->request_stop();
    fixture.source->request_stop();

    const auto stop_deadline = std::chrono::steady_clock::now() + stop_timeout;
    while (std::chrono::steady_clock::now() < stop_deadline) {
      fixture.job->service_pending_arrival();
      static_cast<void>(fixture.job->take_latest());
      static_cast<void>(fixture.job->drain_signals());
      const auto now = std::chrono::steady_clock::now();
      const auto result =
        fixture.render_stack->tick(fixture.overlay->framebuffer_extent(), now);
      if (result.terminal_error || result.device_loss ||
          result.vulkan_device_lost || result.d3_device_removed) {
        return false;
      }
      const auto internal_work_pending =
        !result.presentation_suspended &&
        (result.slots_in_flight != 0 || result.presentations_in_flight != 0);
      if (internal_work_pending &&
          (!result.next_deadline || *result.next_deadline <= now ||
           *result.next_deadline > now + maximum_internal_poll)) {
        return false;
      }
      if (fixture.job->stop_quiescent() &&
          fixture.source->callbacks_quiescent() && result.drain_complete) {
        return true;
      }
      glfwWaitEventsTimeout(wait_seconds(result.next_deadline, now));
    }
    return false;
  }

  [[nodiscard]] bool graphics_preflight_available()
  {
    if (!hardware_consent_present() || !interactive_desktop_available() ||
        !winrt::Windows::Graphics::Capture::GraphicsCaptureSession::
          IsSupported()) {
      return false;
    }
    auto overlay = OverlayWindow::create({64, 64}, BorderPreference::Bordered);
    if (!overlay) {
      throw overlay.error();
    }
    WindowsRenderStack stack{**overlay, resource_generation};
    WindowsSourceRegistry registry;
    WindowsSourceCatalog catalog{registry};
    const auto sources = catalog.enumerate();
    if (!sources) {
      throw sources.error();
    }
    return !sources->empty();
  }

}    // namespace

TEST_CASE("avb0795 V1 case 14: CPU copy capture to nonblocking present",
          "[avb0795-v1][hardware]")
{
  SECTION("region settings fit native client geometry without input")
  {
    // This isolated path owns its source and replica. All source/overlay creation
    // uses no-activate hints; presentation settings stand in for gesture results.
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
    const auto title = L"avb0795RegionFitSource-" + std::to_wstring(GetCurrentProcessId());
    auto source_window = SourceWindowProcess::create(title);
    REQUIRE(source_window);
    WindowsSourceRegistry registry;
    WindowsSourceCatalog catalog{registry};
    const auto sources = catalog.enumerate();
    REQUIRE(sources);
    const auto source = find_controlled_source(*sources, narrow_ascii(source_window->title()));
    REQUIRE(source);
    WindowsRuntimeFactory factory{registry, []() noexcept { glfwPostEmptyEvent(); },
                                   WindowsBridgePreference::ForceCpuBaseline};
    avb::core::ReplicaSettings settings;
    settings.mode = avb::core::OverlayMode::PassThrough;
    auto created = factory.create(*source, settings);
    REQUIRE(created);
    auto runtime = std::move(*created);
    bool stopped{};
    struct Cleanup final {
      avb::app::IReplicaRuntime &runtime;
      bool &stopped;
      ~Cleanup() noexcept
      {
        if (!stopped) {
          bool lone{};
          if (!stop_runtime(runtime, lone)) {
            fail_without_unwind("ERROR: Region-fit runtime cleanup could not prove retirement");
          }
        }
      }
    } cleanup{*runtime, stopped};
    auto *const concrete = dynamic_cast<WindowsReplicaRuntime *>(runtime.get());
    REQUIRE(concrete);
    auto diagnostics = concrete->diagnostics();
    const auto tick_once = [&] {
      const auto now = std::chrono::steady_clock::now();
      const auto tick = runtime->tick(now);
      REQUIRE(event_count<avb::app::Failed>(tick) == 0);
      diagnostics = concrete->diagnostics();
      REQUIRE(diagnostics);
      glfwWaitEventsTimeout(wait_seconds(tick.next_wake, now));
    };
    const auto initial_deadline = std::chrono::steady_clock::now() + presentation_timeout;
    while ((!diagnostics || !diagnostics->presentation) &&
           std::chrono::steady_clock::now() < initial_deadline) {
      tick_once();
    }
    REQUIRE(diagnostics);
    REQUIRE(diagnostics->presentation);
    const auto content = diagnostics->presentation->content;
    REQUIRE(content.width >= 170);
    REQUIRE(content.height >= 110);
    const auto hwnd = process_owned_overlay();
    REQUIRE(hwnd);
    const auto client_rect = [&] {
      RECT rect{};
      REQUIRE(GetClientRect(hwnd, &rect));
      REQUIRE(rect.right > 0);
      REQUIRE(rect.bottom > 0);
      return rect;
    };
    const auto outer_rect = [&] {
      RECT rect{};
      REQUIRE(GetWindowRect(hwnd, &rect));
      return rect;
    };
    const auto same_rect = [](const RECT &a, const RECT &b) {
      return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    };
    const auto check_native_fit = [&](const LONG previous_width, const Extent extent) {
      MONITORINFO monitor{.cbSize = sizeof(MONITORINFO)};
      REQUIRE(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor));
      RECT frame{0, 0, 1, 1};
      REQUIRE(AdjustWindowRectExForDpi(&frame,
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd)));
      const double available_width = monitor.rcWork.right - monitor.rcWork.left -
        (frame.right - frame.left - 1);
      const double available_height = monitor.rcWork.bottom - monitor.rcWork.top -
        (frame.bottom - frame.top - 1);
      const double ratio = static_cast<double>(extent.width) / extent.height;
      const double width = (std::min)({static_cast<double>(previous_width),
                                        available_width, available_height * ratio});
      const auto observed = client_rect();
      REQUIRE(observed.right == (std::max)(1L, std::lround(width)));
      REQUIRE(observed.bottom == (std::max)(1L, std::lround(width / ratio)));
      const auto outer = outer_rect();
      CHECK(outer.left >= monitor.rcWork.left);
      CHECK(outer.top >= monitor.rcWork.top);
      CHECK(outer.right <= monitor.rcWork.right);
      CHECK(outer.bottom <= monitor.rcWork.bottom);
      return observed;
    };
    const auto await_presentation = [&](const avb::core::PixelRegion region) {
      const auto client = client_rect();
      const Extent framebuffer{static_cast<std::uint32_t>(client.right),
                                static_cast<std::uint32_t>(client.bottom)};
      const auto deadline = std::chrono::steady_clock::now() + presentation_timeout;
      while ((!diagnostics->presentation || diagnostics->presentation->region != region ||
              diagnostics->presentation->framebuffer != framebuffer) &&
             std::chrono::steady_clock::now() < deadline) {
        tick_once();
      }
      REQUIRE(diagnostics->presentation);
      const auto &shown = *diagnostics->presentation;
      REQUIRE(shown.region == region);
      REQUIRE(shown.framebuffer == framebuffer);
      REQUIRE_FALSE(shown.selection_full_view);
      // Native dimensions round; aspect_fit truncates its secondary axis.
      // Their at-most-one-pixel spare space is distinct from stale-size bars.
      CHECK(shown.viewport.width <= framebuffer.width);
      CHECK(shown.viewport.height <= framebuffer.height);
      CHECK(framebuffer.width - shown.viewport.width <= 1);
      CHECK(framebuffer.height - shown.viewport.height <= 1);
    };

    const auto initial = client_rect();
    settings.region = avb::core::PixelRegion{10, 20, 160, 90};
    settings.border = BorderPreference::Borderless;
    REQUIRE(runtime->apply(settings));
    const auto cropped = check_native_fit(initial.right, {160, 90});
    await_presentation(*settings.region);

    // General preferences and same-value applications preserve current geometry.
    const auto stable = outer_rect();
    settings.opacity_percent = 75;
    settings.wheel_zoom_percent = 8;
    REQUIRE(runtime->apply(settings));
    REQUIRE(runtime->apply(settings));
    CHECK(same_rect(outer_rect(), stable));

    settings.region.reset();
    settings.border = BorderPreference::Bordered;
    REQUIRE(runtime->apply(settings));
    const auto full = check_native_fit(cropped.right, content);
    await_presentation({0, 0, content.width, content.height});
    const auto restored = outer_rect();
    const auto previous_presented = diagnostics->presented_frames;
    REQUIRE(source_window->request_repaint());
    const auto repaint_deadline = std::chrono::steady_clock::now() + presentation_timeout;
    while (diagnostics->presented_frames <= previous_presented &&
           std::chrono::steady_clock::now() < repaint_deadline) {
      tick_once();
      CHECK(same_rect(outer_rect(), restored));
    }
    REQUIRE(diagnostics->presented_frames > previous_presented);

    settings.region = avb::core::PixelRegion{0, 0, 1, content.height};
    REQUIRE(runtime->apply(settings));
    const auto tall = check_native_fit(full.right, {1, content.height});
    CHECK(tall.right < full.right);
    // A one-pixel source width has deliberately coarse aspect rounding; the
    // normal crop/reset checks above own the one-pixel viewport tolerance.
    bool lone{};
    stopped = stop_runtime(*runtime, lone);
    REQUIRE(stopped);
    CHECK(lone);
    CHECK(source_window->close());
  }

  SECTION("legacy renderer ownership selection and recovery lifecycle")
  {
  // Exercise the actual canonical owner independently of the capture facade.
  // No queue work is submitted; each failure checks a resource/phase boundary.
  {
    INFO("shared renderer installation and producer handoff identity");
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window{
      glfwCreateWindow(64, 64, "avb0795 renderer contract", nullptr, nullptr),
      &glfwDestroyWindow};
    glfwDefaultWindowHints();
    REQUIRE(window);
    avb::render::VulkanRenderer renderer{
      window.get(), resource_generation, {}, {}};
    struct Cleanup final {
      avb::render::VulkanRenderer &renderer;
      std::optional<avb::render::SlotKey> pending;
      ~Cleanup() noexcept
      {
        if (pending) {
          static_cast<void>(renderer.cancel_source_reservation(*pending));
        }
        renderer.begin_drain();
        renderer.detach_for_shutdown();
      }
    } cleanup{renderer, std::nullopt};
    constexpr Extent allocation{16, 16};
    const auto first = renderer.begin_ring(allocation);
    REQUIRE(first);
    CHECK_FALSE(renderer.ready_for_frame());
    CHECK_FALSE(renderer.publish_ring(*first, resource_generation));
    CHECK_FALSE(renderer.create_cpu_slot({*first, 3}));
    for (std::uint32_t index{}; index < 3; ++index) {
      REQUIRE(renderer.create_cpu_slot({*first, index}));
    }
    REQUIRE(renderer.publish_ring(*first, resource_generation));
    CHECK(renderer.ready_for_frame());
    cleanup.pending = renderer.reserve_source_slot();
    REQUIRE(cleanup.pending);
    CHECK_FALSE(renderer.render_allocation_idle());
    CHECK_FALSE(renderer.detach_ring(*first));
    CHECK_FALSE(renderer.cancel_source_reservation(
      {*first + 1, cleanup.pending->index}));
    REQUIRE(renderer.cancel_source_reservation(*cleanup.pending));
    cleanup.pending.reset();
    CHECK(renderer.render_allocation_idle());

    cleanup.pending = renderer.reserve_source_slot();
    REQUIRE(cleanup.pending);
    std::array<std::byte, 16 * 16 * 4> pixels{};
    const avb::render::FrameStamp stamp{
      .sequence = 1, .intake_observed_at = std::chrono::steady_clock::now()};
    const avb::render::CpuRowsView rows{
      .bytes = pixels, .row_pitch = 64,
      .content = allocation, .allocation = allocation};
    auto invalid_rows = rows;
    invalid_rows.row_pitch = 63;
    CHECK_FALSE(renderer.complete_cpu_input(*cleanup.pending, invalid_rows, stamp));
    CHECK_FALSE(renderer.complete_cpu_input(
      {*first + 1, cleanup.pending->index}, rows, stamp));
    CHECK_FALSE(renderer.complete_external_input(
      *cleanup.pending, allocation, stamp, {}));
    REQUIRE(renderer.complete_cpu_input(*cleanup.pending, rows, stamp));
    const auto completed_key = *cleanup.pending;
    cleanup.pending.reset();
    CHECK_FALSE(renderer.complete_cpu_input(completed_key, rows, stamp));
    CHECK(renderer.render_allocation_idle());
    CHECK_FALSE(renderer.detach_ring(*first + 1));
    REQUIRE(renderer.detach_ring(*first));
    renderer.release_ring_resources();
    const auto second = renderer.begin_ring(allocation);
    REQUIRE(second);
    CHECK(*second > *first);
    CHECK_FALSE(renderer.create_cpu_slot(completed_key));
    CHECK_FALSE(renderer.detach_ring(*first));
    REQUIRE(renderer.create_cpu_slot({*second, 0}));
    REQUIRE(renderer.detach_ring(*second));
    renderer.release_ring_resources();
    CHECK_FALSE(renderer.ready_for_frame());
  }

  // Exercise actual producer resources across a same-generation replacement.
  // The stale operations must leave the new ring available for preparation.
  {
    avb::platform::windows::WindowsFrameBridge bridge;
    constexpr Extent allocation{16, 16};
    REQUIRE(bridge.start_ring(7, resource_generation, allocation,
                              DXGI_FORMAT_B8G8R8A8_UNORM));
    CHECK_FALSE(bridge.create_cpu_slot({7, 3}));
    REQUIRE(bridge.create_cpu_slot({7, 0}));
    CHECK_FALSE(bridge.create_cpu_slot({7, 0}));
    REQUIRE(bridge.release_ring(7));
    REQUIRE(bridge.start_ring(8, resource_generation, allocation,
                              DXGI_FORMAT_B8G8R8A8_UNORM));
    CHECK_FALSE(bridge.release_ring(7));
    CHECK_FALSE(bridge.create_cpu_slot({7, 1}));
    REQUIRE(bridge.create_cpu_slot({8, 1}));
    REQUIRE(bridge.release_ring(8));
    CHECK_FALSE(bridge.start_ring(8, resource_generation, allocation,
                                  DXGI_FORMAT_B8G8R8A8_UNORM));
    CHECK_FALSE(bridge.has_pending_inputs());
  }

  const auto controlled_title = L"avb0795GraphicsRuntimeSource-" +
                                std::to_wstring(GetCurrentProcessId());
  auto source_window = SourceWindowProcess::create(controlled_title);
  REQUIRE(source_window.has_value());

  WindowsSourceRegistry runtime_registry;
  WindowsSourceCatalog runtime_catalog{runtime_registry};
  const auto sources = runtime_catalog.enumerate();
  REQUIRE(sources.has_value());
  const auto controlled_source =
    find_controlled_source(*sources, narrow_ascii(source_window->title()));
  REQUIRE(controlled_source.has_value());

  WindowsRuntimeFactory factory{runtime_registry,
                                []() noexcept { glfwPostEmptyEvent(); },
                                WindowsBridgePreference::ForceCpuBaseline};
  auto created = factory.create(*controlled_source, {});
  REQUIRE(created.has_value());
  auto runtime = std::move(*created);
  bool runtime_stopped{};
  bool stopped_event_was_lone{};
  struct RuntimeCleanup final {
    std::unique_ptr<avb::app::IReplicaRuntime> *runtime;
    bool *stopped;
    ~RuntimeCleanup() noexcept
    {
      if (*runtime && !*stopped) {
        bool lone{};
        if (!stop_runtime(**runtime, lone)) {
          fail_without_unwind(
            "ERROR: Case 14 runtime cleanup could not prove retirement");
        }
        *stopped = true;
      }
    }
  } runtime_cleanup{&runtime, &runtime_stopped};
  auto *const concrete = dynamic_cast<WindowsReplicaRuntime *>(runtime.get());
  REQUIRE(concrete != nullptr);

  std::size_t bridge_events{};
  std::size_t exclusion_events{};
  std::size_t source_lost_events{};
  std::optional<avb::app::BridgeModeChanged> initial_bridge_event;
  std::optional<avb::app::RegionSelectionCompleted> region_completion;
  bool counters_monotonic{true};
  bool observed_pending_deadline{};
  std::uint64_t previous_captured{};
  std::uint64_t previous_presented{};
  std::uint64_t previous_dropped{};
  std::size_t maximum_slots_in_flight{};
  std::size_t maximum_presentations_in_flight{};
  const auto tick_once = [&]() {
    const auto now = std::chrono::steady_clock::now();
    const auto tick = runtime->tick(now);
    REQUIRE(event_count<avb::app::Failed>(tick) == 0);
    for (const auto &event : tick.events) {
      if (!event) {
        continue;
      }
      if (const auto *const bridge =
            std::get_if<avb::app::BridgeModeChanged>(&*event)) {
        ++bridge_events;
        REQUIRE_FALSE(initial_bridge_event.has_value());
        initial_bridge_event = *bridge;
      }
      if (const auto *const completed = std::get_if<avb::app::RegionSelectionCompleted>(&*event)) {
        region_completion = *completed;
      }
    }
    exclusion_events +=
      event_count<avb::app::OverlayCaptureExclusionChanged>(tick);
    source_lost_events += event_count<avb::app::SourceLost>(tick);
    const auto diagnostics = concrete->diagnostics();
    REQUIRE(diagnostics.has_value());
    counters_monotonic = counters_monotonic &&
                         diagnostics->captured_frames >= previous_captured &&
                         diagnostics->presented_frames >= previous_presented &&
                         diagnostics->dropped_frames >= previous_dropped;
    previous_captured = diagnostics->captured_frames;
    previous_presented = diagnostics->presented_frames;
    previous_dropped = diagnostics->dropped_frames;
    maximum_slots_in_flight =
      (std::max) (maximum_slots_in_flight, diagnostics->slots_in_flight);
    maximum_presentations_in_flight =
      (std::max) (maximum_presentations_in_flight,
                  diagnostics->presentations_in_flight);
    if (!diagnostics->presentation_suspended &&
        (diagnostics->slots_in_flight != 0 ||
         diagnostics->presentations_in_flight != 0)) {
      REQUIRE(tick.next_wake.has_value());
      CHECK(*tick.next_wake > now);
      CHECK(*tick.next_wake <= now + maximum_internal_poll);
      observed_pending_deadline = true;
    }
    glfwWaitEventsTimeout(wait_seconds(tick.next_wake, now));
    return *diagnostics;
  };

  auto diagnostics = concrete->diagnostics();
  const auto initial_deadline =
    std::chrono::steady_clock::now() + presentation_timeout;
  while ((!diagnostics || !diagnostics->presentation) &&
         std::chrono::steady_clock::now() < initial_deadline) {
    diagnostics = tick_once();
  }
  REQUIRE(diagnostics.has_value());
  REQUIRE(diagnostics->presented_frames != 0);
  REQUIRE(diagnostics->presentation);
  avb::core::ReplicaSettings crop_settings;
  crop_settings.region = avb::core::PixelRegion{10, 20, 160, 90};
  crop_settings.opacity_percent = 50;
  crop_settings.mode = avb::core::OverlayMode::PassThrough;
  REQUIRE(runtime->apply(crop_settings));
  const auto crop_deadline = std::chrono::steady_clock::now() + presentation_timeout;
  while ((!diagnostics->presentation || diagnostics->presentation->region != *crop_settings.region) &&
         std::chrono::steady_clock::now() < crop_deadline) {
    diagnostics = tick_once();
  }
  REQUIRE(diagnostics->presentation);
  CHECK(diagnostics->presentation->region == *crop_settings.region);
  const auto cropped_overlay = process_owned_overlay();
  REQUIRE(cropped_overlay);
  BYTE alpha{};
  DWORD alpha_flags{};
  COLORREF color_key{};
  REQUIRE(GetLayeredWindowAttributes(cropped_overlay, &color_key, &alpha, &alpha_flags));
  CHECK(alpha == 128);
  crop_settings.selecting_region = true;
  REQUIRE(runtime->apply(crop_settings));
  // A pending full view consumes pointer messages without accepting the old crop's mapping.
  SendMessageW(cropped_overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 20));
  SendMessageW(cropped_overlay, WM_LBUTTONUP, 0, MAKELPARAM(60, 60));
  CHECK((GetWindowLongPtrW(cropped_overlay, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0);
  const auto selection_deadline = std::chrono::steady_clock::now() + presentation_timeout;
  while ((!diagnostics->presentation || !diagnostics->presentation->selection_full_view) &&
         std::chrono::steady_clock::now() < selection_deadline) {
    diagnostics = tick_once();
  }
  REQUIRE(diagnostics->presentation);
  REQUIRE(diagnostics->presentation->selection_full_view);
  REQUIRE_FALSE(region_completion);
  RECT selection_client{};
  REQUIRE(GetClientRect(cropped_overlay, &selection_client));
  const auto mapping = *diagnostics->presentation;
  const auto start_x = static_cast<int>((mapping.viewport.x + mapping.viewport.width / 4.0) *
                                        selection_client.right / mapping.framebuffer.width);
  const auto start_y = static_cast<int>((mapping.viewport.y + mapping.viewport.height / 4.0) *
                                        selection_client.bottom / mapping.framebuffer.height);
  const auto end_x = static_cast<int>((mapping.viewport.x + mapping.viewport.width * 0.75) *
                                      selection_client.right / mapping.framebuffer.width);
  const auto end_y = static_cast<int>((mapping.viewport.y + mapping.viewport.height * 0.75) *
                                      selection_client.bottom / mapping.framebuffer.height);
  SendMessageW(cropped_overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(start_x, start_y));
  SendMessageW(cropped_overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(end_x, end_y));
  SendMessageW(cropped_overlay, WM_LBUTTONUP, 0, MAKELPARAM(end_x, end_y));
  const auto completed_deadline = std::chrono::steady_clock::now() + presentation_timeout;
  while (!region_completion && std::chrono::steady_clock::now() < completed_deadline) {
    diagnostics = tick_once();
  }
  REQUIRE(region_completion);
  REQUIRE(region_completion->region);
  CHECK(avb::core::valid_region(*region_completion->region, mapping.content));
  crop_settings.region = region_completion->region;
  crop_settings.selecting_region = false;
  REQUIRE(runtime->apply(crop_settings));
  CHECK((GetWindowLongPtrW(cropped_overlay, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0);
  REQUIRE(GetLayeredWindowAttributes(cropped_overlay, &color_key, &alpha, &alpha_flags));
  CHECK(alpha == 128);
  crop_settings.region.reset();
  REQUIRE(runtime->apply(crop_settings));
  CHECK(diagnostics->bridge_mode == avb::core::BridgeMode::CpuCopy);
  REQUIRE(initial_bridge_event.has_value());
  CHECK(initial_bridge_event->mode == avb::core::BridgeMode::CpuCopy);
  CHECK(initial_bridge_event->reason != avb::core::BridgeReason::None);
  CHECK(bridge_events == 1);
  CHECK(exclusion_events == 1);
  const auto initial_allocation = diagnostics->allocation;
  const auto initial_generation = diagnostics->resource_generation;
  const auto presented_before_resize = diagnostics->presented_frames;

  constexpr Extent resized_client_extent{800, 600};
  REQUIRE(source_window->resize_client(resized_client_extent));
  RECT resized_client{};
  REQUIRE(GetClientRect(source_window->window(), &resized_client) != FALSE);
  CHECK(resized_client.right - resized_client.left == 800);
  CHECK(resized_client.bottom - resized_client.top == 600);
  // Query WGC directly for this fixture-owned HWND: client size and catalog
  // GetWindowRect metadata describe different boundaries from capture content.
  using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
  const auto capture_interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  GraphicsCaptureItem resized_item{nullptr};
  REQUIRE(SUCCEEDED(capture_interop->CreateForWindow(source_window->window(),
    winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
    winrt::put_abi(resized_item))));
  const auto item_size = resized_item.Size();
  REQUIRE(item_size.Width > 0);
  REQUIRE(item_size.Height > 0);
  const Extent resized_allocation{static_cast<std::uint32_t>(item_size.Width),
                                  static_cast<std::uint32_t>(item_size.Height)};

  const auto resize_deadline =
    std::chrono::steady_clock::now() + presentation_timeout;
  bool repainted_new_allocation{};
  while ((diagnostics->allocation != resized_allocation ||
          diagnostics->resource_generation <= initial_generation ||
          diagnostics->presented_frames <= presented_before_resize ||
          !diagnostics->presentation ||
          diagnostics->presentation->content != resized_allocation) &&
         std::chrono::steady_clock::now() < resize_deadline) {
    diagnostics = tick_once();
    if (!repainted_new_allocation && diagnostics->resource_generation > initial_generation) {
      // Supply a known new source update after WGC replaces its allocation.
      REQUIRE(source_window->request_repaint());
      repainted_new_allocation = true;
    }
  }
  CAPTURE(diagnostics->allocation.width, diagnostics->allocation.height,
          diagnostics->resource_generation, initial_generation,
          diagnostics->presented_frames, presented_before_resize);
  CHECK(diagnostics->allocation == resized_allocation);
  CHECK(diagnostics->allocation != initial_allocation);
  CHECK(diagnostics->resource_generation > initial_generation);
  CHECK(diagnostics->presented_frames > presented_before_resize);
  REQUIRE(diagnostics->presentation);
  REQUIRE(diagnostics->presentation->content == resized_allocation);

  crop_settings.region = avb::core::PixelRegion{10, 20, 160, 90};
  REQUIRE(runtime->apply(crop_settings));
  const auto recrop_deadline = std::chrono::steady_clock::now() + presentation_timeout;
  while ((!diagnostics->presentation || diagnostics->presentation->region != *crop_settings.region) &&
         std::chrono::steady_clock::now() < recrop_deadline) {
    diagnostics = tick_once();
  }
  REQUIRE(diagnostics->presentation);
  REQUIRE(diagnostics->presentation->region == *crop_settings.region);

  // A disconnected source cannot supply a replacement frame after the overlay
  // recreates its swapchain. Finish every outstanding copy/present first.
  REQUIRE(source_window->close());
  const auto disconnected_deadline =
    std::chrono::steady_clock::now() + presentation_timeout;
  while ((source_lost_events == 0 || diagnostics->slots_in_flight != 0 ||
          diagnostics->presentations_in_flight != 0) &&
         std::chrono::steady_clock::now() < disconnected_deadline) {
    diagnostics = tick_once();
  }
  REQUIRE(source_lost_events == 1);
  REQUIRE(diagnostics->slots_in_flight == 0);
  REQUIRE(diagnostics->presentations_in_flight == 0);
  const auto captured_after_disconnect = diagnostics->captured_frames;
  // Selecting a new source clears its session-local crop while this disconnected
  // owner remains retained. General preferences still apply to that old overlay.
  crop_settings.region.reset();
  crop_settings.opacity_percent = 25;
  CHECK(runtime->apply(crop_settings));
  const auto retained_reset_deadline = std::chrono::steady_clock::now() + 2s;
  while ((!diagnostics->presentation || diagnostics->presentation->region.width != resized_allocation.width ||
          diagnostics->presentation->region.height != resized_allocation.height ||
          diagnostics->slots_in_flight || diagnostics->presentations_in_flight) &&
         std::chrono::steady_clock::now() < retained_reset_deadline) {
    diagnostics = tick_once();
  }
  REQUIRE(diagnostics->presentation);
  CHECK(diagnostics->presentation->region == avb::core::PixelRegion{0, 0, resized_allocation.width, resized_allocation.height});
  CHECK_FALSE(runtime->tick(std::chrono::steady_clock::now()).next_wake);

  const auto runtime_overlay = process_owned_overlay();
  REQUIRE(runtime_overlay != nullptr);
  ShowWindow(runtime_overlay, SW_MINIMIZE);
  const auto minimized_deadline =
    std::chrono::steady_clock::now() + presentation_timeout;
  while (!diagnostics->presentation_suspended &&
         std::chrono::steady_clock::now() < minimized_deadline) {
    diagnostics = tick_once();
  }
  REQUIRE(diagnostics->presentation_suspended);
  const auto presented_at_suspend = diagnostics->presented_frames;

  ShowWindow(runtime_overlay, SW_RESTORE);
  const auto restored_deadline =
    std::chrono::steady_clock::now() + presentation_timeout;
  while ((diagnostics->presentation_suspended ||
          diagnostics->presented_frames <= presented_at_suspend) &&
         std::chrono::steady_clock::now() < restored_deadline) {
    diagnostics = tick_once();
  }
  CHECK_FALSE(diagnostics->presentation_suspended);
  CHECK(diagnostics->presented_frames > presented_at_suspend);
  CHECK(diagnostics->captured_frames == captured_after_disconnect);
  CHECK(counters_monotonic);
  CHECK(maximum_slots_in_flight <= 3);
  CHECK(maximum_presentations_in_flight <= 3);
  CHECK(observed_pending_deadline);
  CHECK(bridge_events == 1);
  CHECK(exclusion_events == 1);

  runtime_stopped = stop_runtime(*runtime, stopped_event_was_lone);
  if (!runtime_stopped) {
    fail_without_unwind(
      "ERROR: Case 14 runtime stop could not prove native retirement");
  }
  CHECK(stopped_event_was_lone);
  runtime.reset();
  CHECK(source_window->close());
  }
}

TEST_CASE(
  "avb0795 V1 case 15: optional GPU interop capability gate and "
  "presentation",
  "[avb0795-v1][hardware]")
{
  const auto controlled_title = L"avb0795GraphicsInteropSource-" +
                                std::to_wstring(GetCurrentProcessId());
  auto source_window = SourceWindowProcess::create(controlled_title);
  REQUIRE(source_window.has_value());
  auto fixture = create_fixture(narrow_ascii(source_window->title()));
  struct Cleanup final {
    std::unique_ptr<GraphicsFixture> *fixture;
    bool armed{true};
    ~Cleanup() noexcept
    {
      if (armed && *fixture && !stop_fixture(**fixture)) {
        fail_without_unwind(
          "ERROR: Case 15 cleanup could not prove native retirement");
      }
    }
  } cleanup_guard{&fixture};

  const auto facts = fixture->render_stack->bridge_capability_facts();
  INFO("interop facts: luid="
       << facts.adapter_luid_matches
       << " image=" << facts.external_image_supported << " d3d11="
       << facts.required_d3d11_interfaces_available << " shared-timeline="
       << facts.shared_fence_or_timeline_semaphore_available
       << " timeline-import=" << facts.timeline_fence_import_available
       << " win32-procs=" << facts.win32_import_procedure_available
       << " memory-type=" << facts.compatible_memory_type_available);
  const auto capability_selection = avb::render::select_bridge(facts);
  const auto actual_selection = fixture->render_stack->bridge_selection();
  REQUIRE(actual_selection.has_value());
  const auto initial_edge = fixture->render_stack->take_bridge_mode_change();
  REQUIRE(initial_edge.has_value());
  CHECK(*initial_edge == *actual_selection);
  CHECK_FALSE(fixture->render_stack->take_bridge_mode_change().has_value());

  if (actual_selection->reason ==
      avb::core::BridgeReason::InteropOperationFailure) {
    INFO(
      "GPU interop resource setup failed after the capability prefix "
      "passed");
    CHECK(actual_selection->mode == avb::core::BridgeMode::CpuCopy);
    CHECK(facts.adapter_luid_matches);
    CHECK(facts.external_image_supported);
    CHECK(facts.required_d3d11_interfaces_available);
    CHECK(facts.shared_fence_or_timeline_semaphore_available);
    CHECK(facts.timeline_fence_import_available);
    CHECK(facts.win32_import_procedure_available);
  }
  else if (capability_selection.mode ==
           avb::core::BridgeMode::CpuCopy) {
    INFO("GPU interop unsupported; selected CPU copy reason "
         << static_cast<int>(capability_selection.reason));
    CHECK(*actual_selection == capability_selection);
    SUCCEED("automatic CPU fallback matches the real capability facts");
  }
  else {
    CHECK(*actual_selection == capability_selection);
  }

  bool presented{};
  std::optional<avb::render::BridgeSelection> runtime_fallback;
  const auto deadline = std::chrono::steady_clock::now() + presentation_timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    fixture->job->service_pending_arrival();
    const auto signals = fixture->job->drain_signals();
    REQUIRE_FALSE(signals.fatal_active);
    if (fixture->render_stack->ready_for_frame()) {
      if (auto frame = fixture->job->take_latest()) {
        fixture->render_stack->submit(std::move(*frame));
      }
    }

    const auto now = std::chrono::steady_clock::now();
    const auto framebuffer = fixture->overlay->framebuffer_extent();
    fixture->render_stack->resize(framebuffer);
    const auto result = fixture->render_stack->tick(framebuffer, now);
    REQUIRE_FALSE(result.terminal_error.has_value());
    REQUIRE_FALSE(result.device_loss.has_value());
    REQUIRE_FALSE(result.vulkan_device_lost);
    REQUIRE_FALSE(result.d3_device_removed);
    if (const auto edge = fixture->render_stack->take_bridge_mode_change()) {
      REQUIRE_FALSE(runtime_fallback.has_value());
      CHECK(edge->mode == avb::core::BridgeMode::CpuCopy);
      CHECK(edge->reason ==
            avb::core::BridgeReason::InteropOperationFailure);
      runtime_fallback = edge;
    }
    if (result.presented_frames != 0) {
      presented = true;
      break;
    }
    glfwWaitEventsTimeout(wait_seconds(result.next_deadline, now));
  }
  CHECK(presented);
  const auto final_selection = fixture->render_stack->bridge_selection();
  REQUIRE(final_selection.has_value());
  if (actual_selection->mode == avb::core::BridgeMode::GpuInterop &&
      !runtime_fallback) {
    CHECK(final_selection->mode == avb::core::BridgeMode::GpuInterop);
    CHECK(fixture->render_stack->gpu_interop_frames_submitted() != 0);
  }
  else if (runtime_fallback) {
    CHECK(*final_selection == *runtime_fallback);
  }
  else {
    CHECK(fixture->render_stack->gpu_interop_frames_submitted() == 0);
  }
  CHECK_FALSE(fixture->render_stack->take_bridge_mode_change().has_value());

  // Stop submitting source frames, retire all native work, then recreate the
  // swapchain twice using only the retained completed CPU/imported image.
  auto settled = fixture->render_stack->tick(
    fixture->overlay->framebuffer_extent(), std::chrono::steady_clock::now());
  const auto settled_deadline =
    std::chrono::steady_clock::now() + presentation_timeout;
  while ((settled.slots_in_flight != 0 ||
          settled.presentations_in_flight != 0 || settled.cleanup_pending) &&
         std::chrono::steady_clock::now() < settled_deadline) {
    glfwWaitEventsTimeout(wait_seconds(settled.next_deadline,
                                       std::chrono::steady_clock::now()));
    settled = fixture->render_stack->tick(
      fixture->overlay->framebuffer_extent(), std::chrono::steady_clock::now());
    REQUIRE_FALSE(settled.terminal_error);
    REQUIRE_FALSE(settled.device_loss);
  }
  REQUIRE(settled.slots_in_flight == 0);
  REQUIRE(settled.presentations_in_flight == 0);
  REQUIRE_FALSE(settled.cleanup_pending);
  const auto captured_before_redraw = settled.captured_frames;
  REQUIRE(settled.presentation);
  const auto full_content = settled.presentation->content;
  const avb::core::PixelRegion crop{20, 10, 120, 80};
  fixture->render_stack->set_presentation({avb::core::PixelRegion{0, 0, 30, 30}, false, {}});
  fixture->render_stack->set_presentation({crop, false, {}});
  REQUIRE(fixture->render_stack->has_redraw_work());
  const auto before_crop = settled.presented_frames;
  avb::core::FrameScheduler redraw_scheduler{avb::core::FpsCap::Fps30,
                                            std::chrono::steady_clock::now()};
  const auto crop_deadline = std::chrono::steady_clock::now() + presentation_timeout;
  while ((!settled.presentation || settled.presentation->region != crop) &&
         std::chrono::steady_clock::now() < crop_deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (redraw_scheduler.poll(now, false, fixture->render_stack->has_redraw_work()).render) {
      fixture->render_stack->request_redraw();
    }
    settled = fixture->render_stack->tick(fixture->overlay->framebuffer_extent(), now);
    REQUIRE_FALSE(settled.terminal_error);
    glfwWaitEventsTimeout(0.002);
  }
  REQUIRE(settled.presentation);
  CHECK(settled.presentation->region == crop);
  CHECK(settled.presented_frames == before_crop + 1);
  CHECK(settled.captured_frames == captured_before_redraw);
  CHECK_FALSE(fixture->render_stack->has_redraw_work());

  // In-flight geometry from before a zero framebuffer stays invalid through
  // retirement. A subsequent full-view draw provides the new mapping.
  fixture->render_stack->set_presentation({crop, true, crop});
  fixture->render_stack->request_redraw();
  settled = fixture->render_stack->tick(fixture->overlay->framebuffer_extent(),
                                        std::chrono::steady_clock::now());
  settled = fixture->render_stack->tick({}, std::chrono::steady_clock::now());
  CHECK_FALSE(settled.presentation);
  const auto invalid_deadline = std::chrono::steady_clock::now() + presentation_timeout;
  while ((settled.slots_in_flight || settled.presentations_in_flight) &&
         std::chrono::steady_clock::now() < invalid_deadline) {
    glfwWaitEventsTimeout(0.002);
    settled = fixture->render_stack->tick({}, std::chrono::steady_clock::now());
    CHECK_FALSE(settled.presentation);
  }
  const auto full_deadline = std::chrono::steady_clock::now() + presentation_timeout;
  while ((!settled.presentation || !settled.presentation->selection_full_view) &&
         std::chrono::steady_clock::now() < full_deadline) {
    settled = fixture->render_stack->tick(fixture->overlay->framebuffer_extent(),
                                          std::chrono::steady_clock::now());
    REQUIRE_FALSE(settled.terminal_error);
    glfwWaitEventsTimeout(0.002);
  }
  REQUIRE(settled.presentation);
  CHECK(settled.presentation->selection_full_view);
  CHECK(settled.presentation->region == avb::core::PixelRegion{0, 0, full_content.width, full_content.height});
  CHECK(settled.captured_frames == captured_before_redraw);

  const auto redraw_overlay = process_owned_overlay();
  REQUIRE(redraw_overlay != nullptr);
  for (const auto extent : std::array{Extent{700, 400}, Extent{800, 450}}) {
    const auto previously_presented = settled.presented_frames;
    const auto previous_framebuffer = fixture->overlay->framebuffer_extent();
    REQUIRE(SetWindowPos(redraw_overlay, nullptr, 0, 0,
                         static_cast<int>(extent.width),
                         static_cast<int>(extent.height),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
    REQUIRE(fixture->overlay->framebuffer_extent() != previous_framebuffer);
    const auto redraw_deadline =
      std::chrono::steady_clock::now() + presentation_timeout;
    do {
      const auto now = std::chrono::steady_clock::now();
      settled = fixture->render_stack->tick(
        fixture->overlay->framebuffer_extent(), now);
      REQUIRE_FALSE(settled.terminal_error);
      REQUIRE_FALSE(settled.device_loss);
      glfwWaitEventsTimeout(wait_seconds(settled.next_deadline, now));
    } while ((settled.presented_frames <= previously_presented ||
              settled.slots_in_flight != 0 ||
              settled.presentations_in_flight != 0 || settled.cleanup_pending) &&
             std::chrono::steady_clock::now() < redraw_deadline);
    REQUIRE(settled.presented_frames > previously_presented);
    CHECK(settled.captured_frames == captured_before_redraw);
    REQUIRE(settled.slots_in_flight == 0);
    REQUIRE(settled.presentations_in_flight == 0);
    REQUIRE_FALSE(settled.cleanup_pending);
    const auto idle = fixture->render_stack->tick(
      fixture->overlay->framebuffer_extent(), std::chrono::steady_clock::now());
    CHECK_FALSE(idle.next_deadline);
    CHECK(idle.presented_frames == settled.presented_frames);
    CHECK(idle.slots_in_flight == 0);
  }

  // Replace a safely detached capture ring on the same graphics device. This
  // exercises imported-image replacement while the shared timeline/fence and
  // its monotonic target remain alive, then proves a new-generation draw retires.
  REQUIRE(fixture->job->begin_reconfigure());
  const auto callback_deadline = std::chrono::steady_clock::now() + stop_timeout;
  while (!fixture->job->frame_callbacks_quiescent() &&
         std::chrono::steady_clock::now() < callback_deadline) {
    glfwWaitEventsTimeout(0.002);
  }
  REQUIRE(fixture->job->frame_callbacks_quiescent());
  REQUIRE_FALSE(fixture->job->has_latest());
  REQUIRE(fixture->render_stack->render_allocation_idle());
  const auto before_rebuild = settled.presented_frames;
  const auto next_generation = resource_generation + 1;
  const auto allocation = fixture->job->initial_allocation();
  const auto before_rebuild_mode = fixture->render_stack->active_bridge_mode();
  REQUIRE(fixture->job->prepare_reconfigure(allocation, next_generation));
  REQUIRE(fixture->render_stack->rebuild_capture_slots(
    next_generation, allocation, DXGI_FORMAT_B8G8R8A8_UNORM));
  CHECK(fixture->render_stack->generation() == next_generation);
  CHECK(fixture->render_stack->active_bridge_mode() == before_rebuild_mode);
  fixture->job->commit_reconfigure();
  REQUIRE(source_window->request_repaint());
  const auto rebuilt_deadline = std::chrono::steady_clock::now() + presentation_timeout;
  while (settled.presented_frames <= before_rebuild &&
         std::chrono::steady_clock::now() < rebuilt_deadline) {
    fixture->job->service_pending_arrival();
    REQUIRE_FALSE(fixture->job->drain_signals().fatal_active);
    if (fixture->render_stack->ready_for_frame()) {
      if (auto frame = fixture->job->take_latest()) {
        CHECK(frame->resource_generation() == next_generation);
        fixture->render_stack->submit(std::move(*frame));
      }
    }
    const auto now = std::chrono::steady_clock::now();
    settled = fixture->render_stack->tick(fixture->overlay->framebuffer_extent(), now);
    REQUIRE_FALSE(settled.terminal_error);
    REQUIRE_FALSE(settled.device_loss);
    glfwWaitEventsTimeout(wait_seconds(settled.next_deadline, now));
  }
  REQUIRE(settled.presented_frames > before_rebuild);
  REQUIRE(settled.presentation);
  CHECK(settled.presentation->content == allocation);

  const auto stopped = stop_fixture(*fixture);
  if (!stopped) {
    fail_without_unwind(
      "ERROR: Case 15 explicit stop could not prove native retirement");
  }
  cleanup_guard.armed = false;
}

int main(const int argc, char *argv[])
{
  if (argc == 4 && std::string_view{argv[1]} == child_mode) {
    return run_source_window_child(widen_ascii(argv[2]), widen_ascii(argv[3]));
  }

  if (is_discovery_invocation(argc, argv)) {
    return Catch::Session().run(argc, argv);
  }

  try {
    const ApartmentLifetime apartment;
    const GlfwLifetime glfw;
    // Preflight also creates an overlay; set safe creation hints before it.
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
    if (!graphics_preflight_available()) {
      return hardware_skip_exit_code;
    }
    return Catch::Session().run(argc, argv);
  } catch (const AppError &error) {
    if (error.code == AppErrorCode::UnsupportedGraphics ||
        error.code == AppErrorCode::SourceUnavailable) {
      return hardware_skip_exit_code;
    }
    std::cerr << "ERROR: graphics hardware preflight failed ("
              << static_cast<int>(error.code) << ").\n";
    return EXIT_FAILURE;
  } catch (const std::exception &error) {
    std::cerr << "ERROR: graphics hardware preflight failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  } catch (...) {
    std::cerr << "ERROR: graphics hardware preflight failed unexpectedly.\n";
    return EXIT_FAILURE;
  }
}
