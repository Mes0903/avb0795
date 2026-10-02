#include "platform/windows/detail/item_observation_state.hpp"
#include "platform/windows/wgc_capture_job.hpp"
#include "platform/windows/windows_capture_source.hpp"
#include "platform/windows/windows_source_catalog.hpp"
#include "platform/windows/windows_source_registry.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/base.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

  using namespace std::chrono_literals;
  using avb::core::AppErrorCode;
  using avb::core::SourceDescriptor;
  using avb::core::SourceKind;
  using avb::platform::windows::WgcCaptureJob;
  using avb::platform::windows::WindowsCapturedFrame;
  using avb::platform::windows::WindowsCaptureSource;
  using avb::platform::windows::WindowsSourceCatalog;
  using avb::platform::windows::WindowsSourceRegistry;

  constexpr int hardware_skip_exit_code = 77;
  constexpr std::wstring_view consent_environment =
    L"AVB0795_WGC_HARDWARE_CONSENT";
  constexpr std::string_view child_mode = "--avb0795-wgc-source-window";
  constexpr std::wstring_view child_window_class =
    L"avb0795WgcHardwareSourceWindow";
  constexpr auto source_ready_timeout = 5s;
  constexpr auto frame_timeout = 5s;
  constexpr auto close_timeout = 5s;
  constexpr auto quiescence_timeout = 3s;
  constexpr auto late_callback_observation = 100ms;
  constexpr auto prompt_stop_limit = 500ms;
  constexpr std::uint32_t pool_buffer_count = 3;

  enum class HardwarePreflightResult { Available,
                                       Unavailable,
                                       Failed };

  struct LifetimeCounters final {
    std::atomic_size_t sources{};
    std::atomic_size_t jobs{};
    std::atomic_size_t callback_targets{};
    std::atomic_size_t frames{};
  };

  struct WakeSignals final {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic_size_t calls{};

    void wake() noexcept
    {
      calls.fetch_add(1, std::memory_order_acq_rel);
      changed.notify_all();
    }
  };

  struct CallbackTarget final {
    explicit CallbackTarget(LifetimeCounters &counters) noexcept
        : counter_{&counters.callback_targets}
    {
      counter_->fetch_add(1, std::memory_order_relaxed);
    }

    ~CallbackTarget() noexcept
    {
      counter_->fetch_sub(1, std::memory_order_relaxed);
    }

    CallbackTarget(const CallbackTarget &) = delete;
    CallbackTarget &operator=(const CallbackTarget &) = delete;

  private:
    std::atomic_size_t *counter_;
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

  [[nodiscard]] std::wstring widen_ascii(const std::string_view value)
  {
    return {value.begin(), value.end()};
  }

  LRESULT CALLBACK source_window_proc(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) noexcept
  {
    switch (message) {
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

  class UniqueDesktop final {
  public:
    explicit UniqueDesktop(const HDESK value) noexcept : value_{value} {}
    ~UniqueDesktop() noexcept { static_cast<void>(close()); }

    UniqueDesktop(const UniqueDesktop &) = delete;
    UniqueDesktop &operator=(const UniqueDesktop &) = delete;

    [[nodiscard]] explicit operator bool() const noexcept
    {
      return value_ != nullptr;
    }

    [[nodiscard]] bool close() noexcept
    {
      const auto previous = std::exchange(value_, nullptr);
      return previous == nullptr || CloseDesktop(previous) != FALSE;
    }

  private:
    HDESK value_{};
  };

  [[noreturn]] void fail_unproven_child_cleanup() noexcept
  {
    std::cerr << "ERROR: controlled child cleanup could not be proven; "
                 "terminating the hardware-test process.\n";
    std::_Exit(EXIT_FAILURE);
  }

  [[nodiscard]] constexpr int
  child_message_loop_exit_code(const int get_message_result) noexcept
  {
    return get_message_result == -1 ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  static_assert(child_message_loop_exit_code(-1) == EXIT_FAILURE);
  static_assert(child_message_loop_exit_code(0) == EXIT_SUCCESS);

  int run_source_window_child(const std::wstring &ready_event_name,
                              const std::wstring &title) noexcept
  {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = source_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = child_window_class.data();
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return EXIT_FAILURE;
    }

    const auto window =
      CreateWindowExW(0, child_window_class.data(), title.c_str(),
                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640,
                      480, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
      return EXIT_FAILURE;
    }
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    UniqueHandle ready_event{
      OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_event_name.c_str())};
    if (!ready_event) {
      DestroyWindow(window);
      return EXIT_FAILURE;
    }
    if (SetEvent(ready_event.get()) == FALSE) {
      DestroyWindow(window);
      return EXIT_FAILURE;
    }
    if (!ready_event.reset()) {
      DestroyWindow(window);
      return EXIT_FAILURE;
    }

    MSG message{};
    int message_result{};
    while ((message_result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    return child_message_loop_exit_code(message_result);
  }

  class SourceWindowProcess final {
  public:
    SourceWindowProcess() = default;

    ~SourceWindowProcess() noexcept { static_cast<void>(close()); }

    SourceWindowProcess(const SourceWindowProcess &) = delete;
    SourceWindowProcess &operator=(const SourceWindowProcess &) = delete;

    SourceWindowProcess(SourceWindowProcess &&other) noexcept
        : job_{std::move(other.job_)}, process_{std::move(other.process_)}, title_{std::move(other.title_)}, cleanup_succeeded_{other.cleanup_succeeded_} {}

    SourceWindowProcess &operator=(SourceWindowProcess &&other) noexcept
    {
      if (this != &other) {
        cleanup_succeeded_ = close();
        job_ = std::move(other.job_);
        process_ = std::move(other.process_);
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
      const auto event_name = L"Local\\avb0795WgcReady-" + suffix;
      UniqueHandle ready_event{
        CreateEventW(nullptr, TRUE, FALSE, event_name.c_str())};
      if (!ready_event) {
        return std::nullopt;
      }

      std::array<wchar_t, MAX_PATH> executable{};
      const auto executable_length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
      if (executable_length == 0 ||
          executable_length == static_cast<DWORD>(executable.size())) {
        return std::nullopt;
      }

      UniqueHandle job{CreateJobObjectW(nullptr, nullptr)};
      if (!job) {
        return std::nullopt;
      }
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
      job_limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                  &job_limits, sizeof(job_limits)) == FALSE) {
        return std::nullopt;
      }

      std::wstring command_line = L"\"" + std::wstring{executable.data()} +
                                  L"\" " + widen_ascii(child_mode) + L" " +
                                  event_name + L" " + title;
      STARTUPINFOW startup{};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process_info{};
      const auto created = CreateProcessW(
        nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
        nullptr, nullptr, &startup, &process_info);
      if (!created) {
        return std::nullopt;
      }
      UniqueHandle process{process_info.hProcess};
      UniqueHandle thread{process_info.hThread};

      if (AssignProcessToJobObject(job.get(), process.get()) == FALSE) {
        const auto terminated =
          TerminateProcess(process.get(), EXIT_FAILURE) != FALSE;
        const auto waited =
          WaitForSingleObject(process.get(),
                              static_cast<DWORD>(close_timeout.count() *
                                                 1000)) == WAIT_OBJECT_0;
        if (!terminated || !waited) {
          fail_unproven_child_cleanup();
        }
        return std::nullopt;
      }
      if (ResumeThread(thread.get()) == std::numeric_limits<DWORD>::max()) {
        if (!job.reset() ||
            WaitForSingleObject(process.get(),
                                static_cast<DWORD>(close_timeout.count() *
                                                   1000)) != WAIT_OBJECT_0) {
          fail_unproven_child_cleanup();
        }
        return std::nullopt;
      }

      const auto ready = WaitForSingleObject(
        ready_event.get(),
        static_cast<DWORD>(source_ready_timeout.count() * 1000));
      if (ready != WAIT_OBJECT_0) {
        const auto terminated =
          TerminateProcess(process.get(), EXIT_FAILURE) != FALSE;
        auto waited = WaitForSingleObject(
                        process.get(),
                        static_cast<DWORD>(close_timeout.count() * 1000)) ==
                      WAIT_OBJECT_0;
        if (!terminated || !waited) {
          if (!job.reset()) {
            fail_unproven_child_cleanup();
          }
          waited = WaitForSingleObject(process.get(),
                                       static_cast<DWORD>(close_timeout.count() *
                                                          1000)) == WAIT_OBJECT_0;
        }
        if (!waited) {
          fail_unproven_child_cleanup();
        }
        return std::nullopt;
      }

      SourceWindowProcess result;
      result.job_ = std::move(job);
      result.process_ = std::move(process);
      result.title_ = title;
      result.cleanup_succeeded_ = thread.reset() && ready_event.reset();
      return result;
    }

    [[nodiscard]] const std::wstring &title() const noexcept { return title_; }

    [[nodiscard]] bool close() noexcept
    {
      if (!process_) {
        return cleanup_succeeded_;
      }
      auto succeeded = cleanup_succeeded_;
      if (WaitForSingleObject(process_.get(), 0) != WAIT_OBJECT_0) {
        const auto window = FindWindowW(nullptr, title_.c_str());
        if (window == nullptr || PostMessageW(window, WM_CLOSE, 0, 0) == FALSE) {
          succeeded = false;
        }
      }
      auto wait_result = WaitForSingleObject(
        process_.get(), static_cast<DWORD>(close_timeout.count() * 1000));
      if (wait_result != WAIT_OBJECT_0) {
        succeeded =
          TerminateProcess(process_.get(), EXIT_FAILURE) != FALSE && succeeded;
        wait_result = WaitForSingleObject(
          process_.get(), static_cast<DWORD>(close_timeout.count() * 1000));
      }
      if (wait_result != WAIT_OBJECT_0) {
        if (!job_.reset()) {
          fail_unproven_child_cleanup();
        }
        wait_result = WaitForSingleObject(
          process_.get(), static_cast<DWORD>(close_timeout.count() * 1000));
      }
      if (wait_result != WAIT_OBJECT_0) {
        fail_unproven_child_cleanup();
      }
      succeeded = wait_result == WAIT_OBJECT_0 && succeeded;
      cleanup_succeeded_ = process_.reset() && job_.reset() && succeeded;
      return cleanup_succeeded_;
    }

  private:
    UniqueHandle job_;
    UniqueHandle process_;
    std::wstring title_;
    bool cleanup_succeeded_{true};
  };

  [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Device> create_hardware_device()
  {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    const auto result = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      device.GetAddressOf(), &feature_level, context.GetAddressOf());
    if (FAILED(result)) {
      return {};
    }
    return device;
  }

  [[nodiscard]] std::optional<SourceDescriptor>
  find_source(const std::vector<SourceDescriptor> &sources, const SourceKind kind, const std::optional<std::string_view> label = std::nullopt)
  {
    for (const auto &source : sources) {
      if (source.kind == kind && (!label || source.display_label == *label)) {
        return source;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::string narrow_ascii(const std::wstring_view value)
  {
    return {value.begin(), value.end()};
  }

  template <typename Predicate>
  [[nodiscard]] bool
  wait_for_condition(const std::shared_ptr<WakeSignals> &signals,
                     const std::chrono::steady_clock::duration timeout,
                     Predicate predicate)
  {
    try {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      auto observed_calls = signals->calls.load(std::memory_order_acquire);
      while (!predicate()) {
        // GraphicsCaptureItem::Closed needs the creating thread's message pump,
        // even in an MTA with a free-threaded frame pool. Match the desktop host.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
          if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            return false;
          }
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
        if (predicate()) {
          return true;
        }
        std::unique_lock lock{signals->mutex};
        // Before publication no runtime wake is bound; keep servicing messages
        // within the original timeout instead of waiting only on that wake.
        const auto poll_deadline =
          (std::min) (deadline, std::chrono::steady_clock::now() + 50ms);
        signals->changed.wait_until(lock, poll_deadline, [&] {
          return signals->calls.load(std::memory_order_acquire) != observed_calls;
        });
        observed_calls = signals->calls.load(std::memory_order_acquire);
        if (std::chrono::steady_clock::now() >= deadline) {
          return predicate();
        }
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  [[nodiscard]] std::optional<WindowsCapturedFrame>
  wait_for_frame(WgcCaptureJob &job,
                 const std::shared_ptr<WakeSignals> &signals)
  {
    try {
      std::optional<WindowsCapturedFrame> result;
      const auto deadline = std::chrono::steady_clock::now() + frame_timeout;
      auto observed_calls = signals->calls.load(std::memory_order_acquire);
      while (std::chrono::steady_clock::now() < deadline) {
        job.service_pending_arrival();
        result = job.take_latest();
        if (result) {
          return result;
        }

        std::unique_lock lock{signals->mutex};
        const auto poll_deadline =
          (std::min) (deadline, std::chrono::steady_clock::now() + 50ms);
        signals->changed.wait_until(lock, poll_deadline, [&] {
          return signals->calls.load(std::memory_order_acquire) != observed_calls;
        });
        observed_calls = signals->calls.load(std::memory_order_acquire);
      }
      job.service_pending_arrival();
      return job.take_latest();
    } catch (...) {
      return std::nullopt;
    }
  }

  [[nodiscard]] bool wait_for_job_quiescence(WgcCaptureJob &job) noexcept
  {
    const auto deadline = std::chrono::steady_clock::now() + quiescence_timeout;
    while (!job.stop_quiescent() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    return job.stop_quiescent();
  }

  [[nodiscard]] bool
  wait_for_source_quiescence(WindowsCaptureSource &source) noexcept
  {
    const auto deadline = std::chrono::steady_clock::now() + quiescence_timeout;
    while (!source.callbacks_quiescent() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    return source.callbacks_quiescent();
  }

  struct CaptureCallbacks final {
    std::shared_ptr<WakeSignals> signals{std::make_shared<WakeSignals>()};
    std::weak_ptr<CallbackTarget> target_lifetime;
    WindowsCaptureSource::WakeMainLoop source_wake;
    WgcCaptureJob::WakeMainLoop job_wake;
  };

  [[nodiscard]] CaptureCallbacks
  make_capture_callbacks(LifetimeCounters &counters)
  {
    CaptureCallbacks result;
    auto target = std::make_shared<CallbackTarget>(counters);
    result.target_lifetime = target;
    result.source_wake = [signals = result.signals, target] noexcept {
      static_cast<void>(target);
      signals->wake();
    };
    result.job_wake = [signals = result.signals,
                       target = std::move(target)] noexcept {
      static_cast<void>(target);
      signals->wake();
    };
    return result;
  }

  [[nodiscard]] bool no_late_wake(const std::shared_ptr<WakeSignals> &signals,
                                  const std::size_t calls_before_observation)
  {
    try {
      std::unique_lock lock{signals->mutex};
      return !signals->changed.wait_for(lock, late_callback_observation, [&] {
        return signals->calls.load(std::memory_order_acquire) !=
               calls_before_observation;
      });
    } catch (...) {
      return false;
    }
  }

  struct FrameOutcome final {
    bool texture_owned{};
    bool sequence_nonzero{};
    bool generation_nonzero{};
    bool allocation_nonzero{};
    bool content_nonzero{};
    bool texture_extent_nonzero{};

    [[nodiscard]] bool valid() const noexcept
    {
      return texture_owned && sequence_nonzero && generation_nonzero &&
             allocation_nonzero && content_nonzero && texture_extent_nonzero;
    }
  };

  struct StartOutcome final {
    bool succeeded{};
    bool threw{};
    std::optional<AppErrorCode> error;
  };

  [[nodiscard]] StartOutcome start_job(WgcCaptureJob &job) noexcept
  {
    try {
      const auto result = job.start_for_publication();
      if (result) {
        return {.succeeded = true};
      }
      return {.error = result.error().code};
    } catch (...) {
      return {.threw = true};
    }
  }

  [[nodiscard]] FrameOutcome
  observe_frame(const WindowsCapturedFrame &frame) noexcept
  {
    FrameOutcome result;
    try {
      const auto texture = frame.texture();
      result.texture_owned = texture != nullptr;
      if (texture) {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        result.texture_extent_nonzero =
          description.Width != 0 && description.Height != 0;
      }
    } catch (...) {
    }
    result.sequence_nonzero = frame.sequence() != 0;
    result.generation_nonzero = frame.resource_generation() != 0;
    result.allocation_nonzero = frame.allocation_extent().width != 0 &&
                                frame.allocation_extent().height != 0;
    result.content_nonzero =
      frame.content_size().width != 0 && frame.content_size().height != 0;
    return result;
  }

  struct CleanupOutcome final {
    bool stop_returned_promptly{true};
    bool job_quiescent{true};
    bool source_quiescent{true};
    bool callback_target_released{true};
    bool no_late_callbacks{true};
    bool quarantined{};

    [[nodiscard]] bool clean() const noexcept
    {
      return stop_returned_promptly && job_quiescent && source_quiescent &&
             callback_target_released && no_late_callbacks && !quarantined;
    }
  };

  struct QuarantinedCaptureOwners final {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    std::unique_ptr<WindowsCaptureSource> source;
    std::unique_ptr<WgcCaptureJob> job;
    std::shared_ptr<WakeSignals> signals;
  };

  class CaptureOwnerBundle final {
  public:
    CaptureOwnerBundle(Microsoft::WRL::ComPtr<ID3D11Device> device,
                       LifetimeCounters &counters,
                       std::shared_ptr<WakeSignals> signals,
                       std::weak_ptr<CallbackTarget> callback_target)
        : device_{std::move(device)}, counters_{&counters}, signals_{std::move(signals)}, callback_target_{std::move(callback_target)}, quarantine_reserve_{std::make_unique<QuarantinedCaptureOwners>()} {}

    ~CaptureOwnerBundle() noexcept
    {
      if (!cleanup_attempted_) {
        static_cast<void>(cleanup());
      }
    }

    CaptureOwnerBundle(const CaptureOwnerBundle &) = delete;
    CaptureOwnerBundle &operator=(const CaptureOwnerBundle &) = delete;

    void adopt_source(std::unique_ptr<WindowsCaptureSource> source) noexcept
    {
      source_ = std::move(source);
      counters_->sources.fetch_add(1, std::memory_order_relaxed);
    }

    void adopt_job(std::unique_ptr<WgcCaptureJob> job) noexcept
    {
      job_ = std::move(job);
      counters_->jobs.fetch_add(1, std::memory_order_relaxed);
    }

    void adopt_frame(WindowsCapturedFrame frame)
    {
      frame_.emplace(std::move(frame));
      counters_->frames.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] WindowsCaptureSource *source() const noexcept
    {
      return source_.get();
    }

    [[nodiscard]] WgcCaptureJob *job() const noexcept { return job_.get(); }

    [[nodiscard]] const WindowsCapturedFrame *frame() const noexcept
    {
      return frame_ ? &*frame_ : nullptr;
    }

    void release_frame() noexcept
    {
      if (frame_) {
        frame_.reset();
        counters_->frames.fetch_sub(1, std::memory_order_relaxed);
      }
    }

    [[nodiscard]] CleanupOutcome cleanup() noexcept
    {
      if (cleanup_attempted_) {
        return cleanup_outcome_;
      }
      cleanup_attempted_ = true;
      release_frame();

      if (job_) {
        const auto stop_started = std::chrono::steady_clock::now();
        job_->request_stop();
        cleanup_outcome_.stop_returned_promptly =
          std::chrono::steady_clock::now() - stop_started < prompt_stop_limit;
        cleanup_outcome_.job_quiescent = wait_for_job_quiescence(*job_);
      }
      if (source_) {
        source_->request_stop();
        cleanup_outcome_.source_quiescent = wait_for_source_quiescence(*source_);
      }

      if (!cleanup_outcome_.job_quiescent || !cleanup_outcome_.source_quiescent) {
        quarantine_reserve_->device = std::move(device_);
        quarantine_reserve_->source = std::move(source_);
        quarantine_reserve_->job = std::move(job_);
        quarantine_reserve_->signals = signals_;
        static_cast<void>(quarantine_reserve_.release());
        cleanup_outcome_.callback_target_released = false;
        cleanup_outcome_.no_late_callbacks = false;
        cleanup_outcome_.quarantined = true;
        return cleanup_outcome_;
      }

      const auto calls_before_destruction =
        signals_->calls.load(std::memory_order_acquire);
      if (job_) {
        job_.reset();
        counters_->jobs.fetch_sub(1, std::memory_order_relaxed);
      }
      if (source_) {
        source_.reset();
        counters_->sources.fetch_sub(1, std::memory_order_relaxed);
      }
      cleanup_outcome_.callback_target_released = callback_target_.expired();
      cleanup_outcome_.no_late_callbacks =
        no_late_wake(signals_, calls_before_destruction);
      return cleanup_outcome_;
    }

  private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    LifetimeCounters *counters_;
    std::shared_ptr<WakeSignals> signals_;
    std::weak_ptr<CallbackTarget> callback_target_;
    std::unique_ptr<WindowsCaptureSource> source_;
    std::unique_ptr<WgcCaptureJob> job_;
    std::optional<WindowsCapturedFrame> frame_;
    std::unique_ptr<QuarantinedCaptureOwners> quarantine_reserve_;
    CleanupOutcome cleanup_outcome_;
    bool cleanup_attempted_{};
  };

  [[nodiscard]] HardwarePreflightResult preflight_hardware()
  {
    if (!hardware_consent_present()) {
      std::cerr << "SKIP: set AVB0795_WGC_HARDWARE_CONSENT=1 only after "
                   "consenting to capture the interactive desktop.\n";
      return HardwarePreflightResult::Unavailable;
    }

    UniqueDesktop desktop{OpenInputDesktop(0, FALSE, GENERIC_READ)};
    if (!desktop) {
      std::cerr << "SKIP: no interactive input desktop is available.\n";
      return HardwarePreflightResult::Unavailable;
    }
    if (!desktop.close()) {
      std::cerr << "ERROR: closing the interactive desktop handle failed.\n";
      return HardwarePreflightResult::Failed;
    }

    if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::
          IsSupported()) {
      std::cerr << "SKIP: Windows Graphics Capture is unsupported.\n";
      return HardwarePreflightResult::Unavailable;
    }
    if (!create_hardware_device()) {
      std::cerr << "SKIP: no D3D11 hardware device is available.\n";
      return HardwarePreflightResult::Unavailable;
    }

    const auto title =
      L"avb0795WgcPreflight-" + std::to_wstring(GetCurrentProcessId());
    auto source_window = SourceWindowProcess::create(title);
    if (!source_window) {
      std::cerr << "ERROR: the controlled source window could not be created.\n";
      return HardwarePreflightResult::Failed;
    }

    const auto native_window = FindWindowW(nullptr, title.c_str());
    if (native_window == nullptr) {
      static_cast<void>(source_window->close());
      std::cerr << "ERROR: the controlled source HWND could not be located.\n";
      return HardwarePreflightResult::Failed;
    }

    bool native_window_capture_available{};
    try {
      const auto interop = winrt::get_activation_factory<
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();
      winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
      native_window_capture_available = SUCCEEDED(interop->CreateForWindow(
        native_window,
        winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        winrt::put_abi(item)));
    } catch (const winrt::hresult_error &) {
      native_window_capture_available = false;
    }

    WindowsSourceRegistry registry;
    WindowsSourceCatalog catalog{registry};
    const auto sources = catalog.enumerate();
    const auto catalog_has_controlled_window =
      sources && find_source(*sources, SourceKind::Window, narrow_ascii(title));
    const auto child_cleanup_succeeded = source_window->close();
    if (!child_cleanup_succeeded) {
      std::cerr << "ERROR: controlled source cleanup failed during preflight.\n";
      return HardwarePreflightResult::Failed;
    }
    if (!sources) {
      std::cerr << "ERROR: source catalog enumeration failed during preflight.\n";
      return HardwarePreflightResult::Failed;
    }
    if (!native_window_capture_available) {
      std::cerr << "SKIP: native WGC cannot capture the controlled window.\n";
      return HardwarePreflightResult::Unavailable;
    }
    if (!catalog_has_controlled_window) {
      std::cerr << "ERROR: native WGC accepts the controlled window, but the "
                   "source catalog omitted it.\n";
      return HardwarePreflightResult::Failed;
    }
    return HardwarePreflightResult::Available;
  }

  class ApartmentRegistration final {
  public:
    ApartmentRegistration() = default;
    ~ApartmentRegistration() noexcept { winrt::uninit_apartment(); }

    ApartmentRegistration(const ApartmentRegistration &) = delete;
    ApartmentRegistration &operator=(const ApartmentRegistration &) = delete;
  };

}    // namespace

TEST_CASE("avb0795 V1 case 13: WGC catalog identity capture and clean stop",
          "[avb0795-v1][hardware]")
{
  LifetimeCounters counters;
  const auto device = create_hardware_device();
  REQUIRE(device);

  SECTION("consented display or controlled window fallback repeats cleanly")
  {
    WindowsSourceRegistry registry;
    WindowsSourceCatalog catalog{registry};
    auto sources = catalog.enumerate();
    REQUIRE(sources.has_value());
    auto repeated_source = find_source(*sources, SourceKind::Display);
    std::optional<SourceWindowProcess> fallback_window;
    if (!repeated_source) {
      const auto fallback_title = L"avb0795WgcRepeatedFallback-" +
                                  std::to_wstring(GetCurrentProcessId());
      fallback_window = SourceWindowProcess::create(fallback_title);
      REQUIRE(fallback_window.has_value());
      sources = catalog.enumerate();
      REQUIRE(sources.has_value());
      repeated_source = find_source(*sources, SourceKind::Window,
                                    narrow_ascii(fallback_title));
    }
    REQUIRE(repeated_source.has_value());

    const auto *const repeated_source_description =
      repeated_source->kind == SourceKind::Display
        ? "Repeated sample uses the consented display source"
        : "Repeated sample uses the controlled window fallback";
    INFO(repeated_source_description);
    for (std::uint64_t sample = 1; sample <= 3; ++sample) {
      CAPTURE(sample);
      auto lease = registry.resolve(repeated_source->id);
      REQUIRE(lease.has_value());

      auto callbacks = make_capture_callbacks(counters);
      CaptureOwnerBundle owners{device, counters, callbacks.signals,
                                callbacks.target_lifetime};
      auto source_result = WindowsCaptureSource::create(
        std::move(*lease), std::move(callbacks.source_wake));
      const auto source_created = source_result.has_value();
      bool source_open{};
      bool job_created{};
      StartOutcome start;
      bool frame_arrived{};
      FrameOutcome frame_observation;

      if (source_result) {
        owners.adopt_source(std::move(*source_result));
        source_open = !owners.source()->is_closed();
        auto job_result = WgcCaptureJob::create_inactive(
          *owners.source(), *device.Get(), sample, pool_buffer_count,
          std::move(callbacks.job_wake));
        job_created = job_result.has_value();
        if (job_result) {
          owners.adopt_job(std::move(*job_result));
          start = start_job(*owners.job());
          if (start.succeeded) {
            owners.source()->activate_runtime_wake();
            auto frame = wait_for_frame(*owners.job(), callbacks.signals);
            frame_arrived = frame.has_value();
            if (frame) {
              owners.adopt_frame(std::move(*frame));
              frame_observation = observe_frame(*owners.frame());
              owners.release_frame();
            }
          }
        }
      }
      callbacks.source_wake = {};
      callbacks.job_wake = {};
      const auto cleanup = owners.cleanup();

      CHECK(source_created);
      CHECK(source_open);
      CHECK(job_created);
      CHECK(start.succeeded);
      CHECK_FALSE(start.threw);
      CHECK(frame_arrived);
      CHECK(frame_observation.valid());
      CHECK(cleanup.stop_returned_promptly);
      CHECK(cleanup.job_quiescent);
      CHECK(cleanup.source_quiescent);
      CHECK(cleanup.callback_target_released);
      CHECK(cleanup.no_late_callbacks);
      CHECK_FALSE(cleanup.quarantined);
      CHECK(counters.sources.load(std::memory_order_relaxed) == 0);
      CHECK(counters.jobs.load(std::memory_order_relaxed) == 0);
      CHECK(counters.callback_targets.load(std::memory_order_relaxed) == 0);
      CHECK(counters.frames.load(std::memory_order_relaxed) == 0);
      if (cleanup.quarantined) {
        break;
      }
    }
    const auto fallback_cleanup = !fallback_window || fallback_window->close();
    CHECK(fallback_cleanup);
  }

  SECTION("controlled window closes before start without retargeting")
  {
    const auto title = L"avb0795WgcClosedBeforeStart-" +
                       std::to_wstring(GetCurrentProcessId());
    auto original_window = SourceWindowProcess::create(title);
    REQUIRE(original_window.has_value());

    WindowsSourceRegistry registry;
    WindowsSourceCatalog catalog{registry};
    const auto sources = catalog.enumerate();
    REQUIRE(sources.has_value());
    const auto selected =
      find_source(*sources, SourceKind::Window, narrow_ascii(title));
    REQUIRE(selected.has_value());
    auto lease = registry.resolve(selected->id);
    REQUIRE(lease.has_value());

    auto callbacks = make_capture_callbacks(counters);
    CaptureOwnerBundle owners{device, counters, callbacks.signals,
                              callbacks.target_lifetime};
    auto source_result = WindowsCaptureSource::create(
      std::move(*lease), std::move(callbacks.source_wake));
    const auto source_created = source_result.has_value();
    bool job_created{};
    bool original_cleanup{};
    bool close_observed{};
    bool replacement_created{};
    bool first_closed_edge{};
    bool second_closed_edge{};
    StartOutcome start;
    std::optional<SourceWindowProcess> replacement_window;

    if (source_result) {
      owners.adopt_source(std::move(*source_result));
      auto job_result = WgcCaptureJob::create_inactive(
        *owners.source(), *device.Get(), 41, pool_buffer_count,
        std::move(callbacks.job_wake));
      job_created = job_result.has_value();
      if (job_result) {
        owners.adopt_job(std::move(*job_result));
        original_cleanup = original_window->close();
        close_observed =
          wait_for_condition(callbacks.signals, close_timeout,
                             [&] { return owners.source()->is_closed(); });
        replacement_window = SourceWindowProcess::create(title);
        replacement_created = replacement_window.has_value();
        start = start_job(*owners.job());
        first_closed_edge = owners.source()->consume_closed_edge();
        second_closed_edge = owners.source()->consume_closed_edge();
      }
    }
    callbacks.source_wake = {};
    callbacks.job_wake = {};
    const auto cleanup = owners.cleanup();
    const auto replacement_cleanup =
      !replacement_window || replacement_window->close();

    CHECK(source_created);
    CHECK(job_created);
    CHECK(original_cleanup);
    CHECK(close_observed);
    CHECK(replacement_created);
    CHECK_FALSE(start.succeeded);
    CHECK_FALSE(start.threw);
    REQUIRE(start.error.has_value());
    CHECK(*start.error == AppErrorCode::SourceUnavailable);
    CHECK(first_closed_edge);
    CHECK_FALSE(second_closed_edge);
    CHECK(cleanup.clean());
    CHECK(replacement_cleanup);
    CHECK(counters.sources.load(std::memory_order_relaxed) == 0);
    CHECK(counters.jobs.load(std::memory_order_relaxed) == 0);
    CHECK(counters.callback_targets.load(std::memory_order_relaxed) == 0);
    CHECK(counters.frames.load(std::memory_order_relaxed) == 0);
  }

  SECTION("controlled source close after publication becomes SourceLost")
  {
    const auto title =
      L"avb0795WgcPostStartClose-" + std::to_wstring(GetCurrentProcessId());
    auto source_window = SourceWindowProcess::create(title);
    REQUIRE(source_window.has_value());

    WindowsSourceRegistry registry;
    WindowsSourceCatalog catalog{registry};
    const auto sources = catalog.enumerate();
    REQUIRE(sources.has_value());
    const auto selected =
      find_source(*sources, SourceKind::Window, narrow_ascii(title));
    REQUIRE(selected.has_value());
    auto lease = registry.resolve(selected->id);
    REQUIRE(lease.has_value());

    auto callbacks = make_capture_callbacks(counters);
    CaptureOwnerBundle owners{device, counters, callbacks.signals,
                              callbacks.target_lifetime};
    auto source_result = WindowsCaptureSource::create(
      std::move(*lease), std::move(callbacks.source_wake));
    const auto source_created = source_result.has_value();
    bool job_created{};
    StartOutcome start;
    bool frame_arrived{};
    FrameOutcome frame_observation;
    bool window_cleanup{};
    bool close_observed{};
    bool first_closed_edge{};
    bool second_closed_edge{};

    if (source_result) {
      owners.adopt_source(std::move(*source_result));
      auto job_result = WgcCaptureJob::create_inactive(
        *owners.source(), *device.Get(), 73, pool_buffer_count,
        std::move(callbacks.job_wake));
      job_created = job_result.has_value();
      if (job_result) {
        owners.adopt_job(std::move(*job_result));
        start = start_job(*owners.job());
        if (start.succeeded) {
          owners.source()->activate_runtime_wake();
          auto frame = wait_for_frame(*owners.job(), callbacks.signals);
          frame_arrived = frame.has_value();
          if (frame) {
            owners.adopt_frame(std::move(*frame));
            frame_observation = observe_frame(*owners.frame());
            owners.release_frame();
          }
          window_cleanup = source_window->close();
          close_observed =
            wait_for_condition(callbacks.signals, close_timeout,
                               [&] { return owners.source()->is_closed(); });
          first_closed_edge = owners.source()->consume_closed_edge();
          second_closed_edge = owners.source()->consume_closed_edge();
        }
      }
    }
    callbacks.source_wake = {};
    callbacks.job_wake = {};
    const auto cleanup = owners.cleanup();
    if (!window_cleanup) {
      window_cleanup = source_window->close();
    }

    CHECK(source_created);
    CHECK(job_created);
    CHECK(start.succeeded);
    CHECK_FALSE(start.threw);
    CHECK(frame_arrived);
    CHECK(frame_observation.valid());
    CHECK(window_cleanup);
    CHECK(close_observed);
    CHECK(first_closed_edge);
    CHECK_FALSE(second_closed_edge);
    CHECK(cleanup.clean());
    CHECK(counters.sources.load(std::memory_order_relaxed) == 0);
    CHECK(counters.jobs.load(std::memory_order_relaxed) == 0);
    CHECK(counters.callback_targets.load(std::memory_order_relaxed) == 0);
    CHECK(counters.frames.load(std::memory_order_relaxed) == 0);
  }

  CHECK(counters.sources.load(std::memory_order_relaxed) == 0);
  CHECK(counters.jobs.load(std::memory_order_relaxed) == 0);
  CHECK(counters.callback_targets.load(std::memory_order_relaxed) == 0);
  CHECK(counters.frames.load(std::memory_order_relaxed) == 0);
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
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (const winrt::hresult_error &error) {
    std::cerr << "ERROR: WGC apartment initialization failed with HRESULT "
              << std::hex << error.code().value << ".\n";
    return EXIT_FAILURE;
  } catch (...) {
    std::cerr << "ERROR: WGC apartment initialization failed unexpectedly.\n";
    return EXIT_FAILURE;
  }
  const ApartmentRegistration apartment;

  try {
    const auto preflight = preflight_hardware();
    if (preflight == HardwarePreflightResult::Unavailable) {
      return hardware_skip_exit_code;
    }
    if (preflight == HardwarePreflightResult::Failed) {
      return EXIT_FAILURE;
    }
  } catch (const winrt::hresult_error &error) {
    std::cerr << "ERROR: WGC hardware preflight failed with HRESULT "
              << std::hex << error.code().value << ".\n";
    return EXIT_FAILURE;
  } catch (const std::exception &error) {
    std::cerr << "ERROR: WGC hardware preflight failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  } catch (...) {
    std::cerr << "ERROR: WGC hardware preflight failed unexpectedly.\n";
    return EXIT_FAILURE;
  }
  return Catch::Session().run(argc, argv);
}
