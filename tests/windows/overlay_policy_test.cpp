#include "platform/windows/capture_exclusion.hpp"
#include "platform/windows/detail/overlay_callback.hpp"
#include "platform/windows/detail/overlay_policy.hpp"
#include "platform/windows/detail/overlay_transactions.hpp"
#include "platform/windows/detail/windows_runtime_policy.hpp"
#include "platform/windows/features/detail/browser_capture_keep_alive_policy.hpp"
#include "platform/windows/overlay_window.hpp"
#include "platform/windows/win32_overlay_adapter.hpp"
#include "platform/windows/windows_controller_window.hpp"

#include "avb/core/types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

  namespace overlay = avb::platform::windows;
  namespace detail = avb::platform::windows::detail;
  namespace helper = avb::platform::windows::features::detail;
  namespace render = avb::render;
  using avb::core::AppError;
  using avb::core::AppErrorCode;
  using avb::core::BorderPreference;
  using avb::core::Extent;
  using avb::core::OverlayMode;

  [[nodiscard]] constexpr AppError platform_failure() noexcept
  {
    return {.code = AppErrorCode::PlatformOperationFailure};
  }

  /// Keep GLFW initialized until every hidden fixture and borrowed adapter retires.
  class TestGlfwLifetime final {
  public:
    TestGlfwLifetime() noexcept : initialized_{glfwInit() == GLFW_TRUE} {}
    ~TestGlfwLifetime() noexcept
    {
      if (initialized_) {
        glfwTerminate();
      }
    }
    TestGlfwLifetime(const TestGlfwLifetime &) = delete;
    TestGlfwLifetime &operator=(const TestGlfwLifetime &) = delete;
    TestGlfwLifetime(TestGlfwLifetime &&) = delete;
    TestGlfwLifetime &operator=(TestGlfwLifetime &&) = delete;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

  private:
    bool initialized_{};
  };

  /// Observe this test thread's sole replica HWND without widening OverlayWindow's API.
  [[nodiscard]] HWND find_test_overlay_window() noexcept
  {
    HWND found{};
    static_cast<void>(EnumThreadWindows(
      GetCurrentThreadId(),
      [](const HWND window, const LPARAM context) noexcept -> BOOL {
        std::array<wchar_t, 64> title{};
        const auto length = GetWindowTextW(window, title.data(),
                                           static_cast<int>(title.size()));
        if (length > 0 &&
            std::wstring_view{title.data(), static_cast<std::size_t>(length)} ==
              L"avb0795 Replica") {
          *reinterpret_cast<HWND *>(context) = window;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&found)));
    return found;
  }

  enum class NativeCall {
    ReadStyle,
    ReadLayered,
    WriteStyle,
    WriteLayered,
  };

  /// Exercise the actual subclass with owned, hidden HWNDs and real capture.
  template <class Scenario>
  void with_region_overlay(Scenario scenario)
  {
    const TestGlfwLifetime glfw;
    REQUIRE(glfw.initialized());
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    constexpr Extent extent{320, 240};
    auto window = overlay::OverlayWindow::create(extent, BorderPreference::Borderless);
    REQUIRE(window.has_value());
    const auto hwnd = find_test_overlay_window();
    REQUIRE(hwnd != nullptr);
    REQUIRE_FALSE(IsWindowVisible(hwnd));
    auto detached = overlay::Win32OverlayAdapter::create_detached(
      **window, OverlayMode::Editable, BorderPreference::Borderless, extent);
    REQUIRE(detached.has_value());
    const auto retire = [&](overlay::Win32OverlayAdapter *borrowed) noexcept {
      std::unique_ptr<overlay::Win32OverlayAdapter> owner{borrowed};
      if (owner->window_destruction_required() || !owner->detach()) {
        (*window)->destroy();
        owner->complete_after_overlay_destroyed();
      }
    };
    const std::unique_ptr<overlay::Win32OverlayAdapter, decltype(retire)>
      adapter{detached->release(), retire};
    REQUIRE(adapter->attach().has_value());
    scenario(hwnd, *adapter);
  }

  struct RecordedNativeCall final {
    NativeCall call{};
    std::uintptr_t style{};
    std::optional<detail::LayeredAttributes> layered;
  };

  class RecordingNativeWindowOperations final {
  public:
    detail::NativeWindowState state;
    std::vector<RecordedNativeCall> calls;
    std::optional<NativeCall> fail_once;
    bool layered_state_readable{true};
    bool corrupt_rollback_verification{};

    [[nodiscard]] std::expected<std::uintptr_t, AppError>
    read_extended_style() noexcept
    {
      calls.push_back({.call = NativeCall::ReadStyle});
      if (consume_failure(NativeCall::ReadStyle)) {
        return std::unexpected{platform_failure()};
      }
      if (corrupt_rollback_verification && wrote_rollback_) {
        return state.extended_style ^ std::uintptr_t{0x100};
      }
      return state.extended_style;
    }

    [[nodiscard]] std::expected<detail::LayeredAttributes, AppError>
    read_layered_attributes() noexcept
    {
      calls.push_back({.call = NativeCall::ReadLayered});
      if (!layered_state_readable || consume_failure(NativeCall::ReadLayered) ||
          !state.layered_attributes) {
        return std::unexpected{platform_failure()};
      }
      return *state.layered_attributes;
    }

    [[nodiscard]] std::expected<void, AppError>
    write_extended_style(const std::uintptr_t style) noexcept
    {
      calls.push_back({.call = NativeCall::WriteStyle, .style = style});
      if (consume_failure(NativeCall::WriteStyle)) {
        return std::unexpected{platform_failure()};
      }
      if (write_count_ > 0) {
        wrote_rollback_ = true;
      }
      ++write_count_;
      state.extended_style = style;
      if (!detail::has_layered_style(style)) {
        state.layered_attributes.reset();
      }
      return {};
    }

    [[nodiscard]] std::expected<void, AppError> write_layered_attributes(
      const detail::LayeredAttributes attributes) noexcept
    {
      calls.push_back({.call = NativeCall::WriteLayered, .layered = attributes});
      if (consume_failure(NativeCall::WriteLayered)) {
        return std::unexpected{platform_failure()};
      }
      state.layered_attributes = attributes;
      return {};
    }

  private:
    [[nodiscard]] bool consume_failure(const NativeCall call) noexcept
    {
      if (fail_once != call) {
        return false;
      }
      fail_once.reset();
      return true;
    }

    std::uint32_t write_count_{};
    bool wrote_rollback_{};
  };

  enum class SettingsCall {
    Snapshot,
    Mode,
    Border,
    Geometry,
    Extensions,
    VerifyApplied,
    Rollback,
    VerifyRollback,
    CommitScheduler,
  };

  enum class SettingsRollbackCall { Helper,
                                    Mode,
                                    Border,
                                    Geometry };

  class RecordingSettingsOperations final {
  public:
    struct Snapshot final {
      int settings{};
      bool helper_active{};
    };

    std::vector<SettingsCall> calls;
    std::vector<SettingsRollbackCall> rollback_calls;
    std::optional<SettingsCall> fail;
    std::optional<SettingsCall> throw_from;
    bool rollback_succeeds{true};
    bool rollback_verifies{true};
    helper::HelperActivationFacts helper_facts{
      .requested = true,
      .window_capture = true,
      .published_tick_ready = true,
      .source_connected = true,
    };
    bool helper_active{};
    bool source_closes_before_extensions{};
    std::size_t helper_show_count{};
    std::size_t helper_hide_count{};

    [[nodiscard]] std::optional<Snapshot> snapshot()
    {
      calls.push_back(SettingsCall::Snapshot);
      maybe_throw(SettingsCall::Snapshot);
      return fail == SettingsCall::Snapshot ? std::nullopt
                                            : std::optional<Snapshot>{
                                                {.settings = 17,
                                                 .helper_active = helper_active}};
    }

    [[nodiscard]] bool apply_mode() { return run_stage(SettingsCall::Mode); }
    [[nodiscard]] bool apply_border() { return run_stage(SettingsCall::Border); }
    [[nodiscard]] bool apply_geometry()
    {
      return run_stage(SettingsCall::Geometry);
    }
    [[nodiscard]] bool apply_extensions()
    {
      calls.push_back(SettingsCall::Extensions);
      maybe_throw(SettingsCall::Extensions);
      if (source_closes_before_extensions) {
        helper_facts.source_connected = false;
      }
      set_helper_to_policy_target();
      return fail != SettingsCall::Extensions;
    }
    [[nodiscard]] bool verify_applied()
    {
      return run_stage(SettingsCall::VerifyApplied);
    }

    [[nodiscard]] bool rollback(const Snapshot snapshot) noexcept
    {
      calls.push_back(SettingsCall::Rollback);
      rollback_calls.push_back(SettingsRollbackCall::Helper);
      helper_facts.requested = snapshot.helper_active;
      set_helper_to_policy_target();
      rollback_calls.push_back(SettingsRollbackCall::Mode);
      rollback_calls.push_back(SettingsRollbackCall::Border);
      rollback_calls.push_back(SettingsRollbackCall::Geometry);
      return snapshot.settings == 17 && rollback_succeeds;
    }

    [[nodiscard]] bool verify_rollback(const Snapshot snapshot) noexcept
    {
      calls.push_back(SettingsCall::VerifyRollback);
      const bool original_helper_is_eligible =
        !snapshot.helper_active || helper::helper_should_be_active(helper_facts);
      return snapshot.settings == 17 && rollback_verifies &&
             original_helper_is_eligible &&
             helper_active == snapshot.helper_active;
    }

    void commit_scheduler() noexcept
    {
      calls.push_back(SettingsCall::CommitScheduler);
    }

  private:
    void set_helper_to_policy_target() noexcept
    {
      const bool target = helper::helper_should_be_active(helper_facts);
      if (helper_active == target) {
        return;
      }
      helper_active = target;
      if (target) {
        ++helper_show_count;
      }
      else {
        ++helper_hide_count;
      }
    }

    [[nodiscard]] bool run_stage(const SettingsCall call)
    {
      calls.push_back(call);
      maybe_throw(call);
      return fail != call;
    }

    void maybe_throw(const SettingsCall call) const
    {
      if (throw_from == call) {
        throw std::runtime_error{"controlled settings failure"};
      }
    }
  };

  struct PreparedMessage final {
    LRESULT result{};
    bool forward_after_commit{};
    std::optional<std::uint32_t> dpi;
  };

  class RecordingCallbackOperations final {
  public:
    bool throw_while_preparing{};
    bool handle_message{true};
    PreparedMessage prepared{.result = 91};
    bool fail_rect_application{};
    std::uint32_t observed_dpi{96};
    std::uint32_t committed_mutations{};
    std::uint32_t forwards{};
    bool binding_live{true};
    std::vector<int> order;

    [[nodiscard]] std::optional<PreparedMessage> prepare(UINT, WPARAM, LPARAM)
    {
      order.push_back(1);
      if (throw_while_preparing) {
        throw std::runtime_error{"controlled callback failure"};
      }
      if (!handle_message) {
        return std::nullopt;
      }
      return prepared;
    }

    [[nodiscard]] std::optional<LRESULT>
    commit(const PreparedMessage &message) noexcept
    {
      order.push_back(2);
      if (fail_rect_application) {
        return std::nullopt;
      }
      ++committed_mutations;
      if (message.dpi) {
        observed_dpi = *message.dpi == 0 ? 96 : *message.dpi;
      }
      return message.result;
    }

    [[nodiscard]] bool
    forward_after_commit(const PreparedMessage &message) const noexcept
    {
      return message.forward_after_commit;
    }

    [[nodiscard]] LRESULT forward(UINT, WPARAM, LPARAM) noexcept
    {
      order.push_back(3);
      ++forwards;
      return 73;
    }

    void clear_binding() noexcept
    {
      order.push_back(4);
      binding_live = false;
    }
  };

  class RecordingAttachmentOperations final {
  public:
    bool restore_wndproc_succeeds{true};
    bool restored_expected_proc{true};
    detail::OverlayPropertyRemoval property_removal{
      detail::OverlayPropertyRemoval::RemovedExpected};
    bool adapter_alive{true};
    bool callback_owned{true};
    bool property_live{true};
    bool window_destruction_required{};
    bool adapter_was_alive_during_destroy{};
    std::optional<AppErrorCode> detach_error;
    std::vector<int> order;
    RecordingCallbackOperations callback{.handle_message = false};

    [[nodiscard]] std::expected<bool, AppError> restore_window_proc() noexcept
    {
      if (!restore_wndproc_succeeds) {
        return std::unexpected{platform_failure()};
      }
      return restored_expected_proc;
    }

    void publish_callback_detached() noexcept { callback_owned = false; }

    [[nodiscard]] detail::OverlayPropertyRemoval
    remove_and_observe_property() noexcept
    {
      if (property_removal != detail::OverlayPropertyRemoval::StillPresent) {
        property_live = false;
      }
      return property_removal;
    }

    void require_window_destruction() noexcept
    {
      window_destruction_required = true;
    }

    [[nodiscard]] bool destroy_with_adapter_alive_required() const noexcept
    {
      return window_destruction_required;
    }

    [[nodiscard]] bool detach_adapter() noexcept
    {
      order.push_back(1);
      const auto result = detail::detach_overlay_subclass(*this);
      if (!result) {
        detach_error = result.error().code;
      }
      return result.has_value();
    }

    void destroy_overlay() noexcept
    {
      order.push_back(2);
      adapter_was_alive_during_destroy = adapter_alive;
      if (callback_owned && property_live) {
        static_cast<void>(
          detail::dispatch_overlay_callback(callback, WM_NCDESTROY, 0, 0));
        callback_owned = callback.binding_live;
      }
      property_live = false;
    }

    void complete_adapter_after_overlay_destroyed() noexcept
    {
      order.push_back(3);
      callback_owned = false;
      window_destruction_required = false;
    }
  };

  enum class HelperCall { Observe,
                          Prepare,
                          Create,
                          Show,
                          Hide,
                          Verify,
                          Destroy,
                          Unregister };

  struct RecordingHelperOperations final {
    std::array<HelperCall, 64> calls{};
    std::size_t call_count{};
    bool window{};
    bool window_class{};
    bool active{};
    bool readable{true};
    bool fail_prepare{};
    bool fail_write_once{};
    bool fail_verify_once{};
    bool fail_verify_always{};
    bool fail_destroy{};
    bool fail_unregister{};
    helper::HelperRetirementState retirement;
    helper::Clock::time_point native_completion{};

    void record(HelperCall call) noexcept
    {
      if (call_count == calls.size()) {
        std::terminate();
      }
      calls[call_count++] = call;
    }

    [[nodiscard]] std::size_t count(HelperCall call) const noexcept
    {
      return static_cast<std::size_t>(std::count(calls.begin(), calls.begin() + call_count, call));
    }

    [[nodiscard]] std::expected<bool, AppError> observe_active() noexcept
    {
      record(HelperCall::Observe);
      if (!readable) {
        return std::unexpected{AppError{.code = AppErrorCode::RuntimeStateLost}};
      }
      return active;
    }

    [[nodiscard]] std::expected<void, AppError> prepare_hidden() noexcept
    {
      record(HelperCall::Prepare);
      if (fail_prepare) {
        return std::unexpected{platform_failure()};
      }
      if (!window) {
        record(HelperCall::Create);
        window = true;
        window_class = true;
      }
      return {};
    }

    [[nodiscard]] std::expected<void, AppError> write_active(bool value) noexcept
    {
      if (!window || (active == value && retirement.hidden_verified == !value)) {
        return {};
      }
      record(value ? HelperCall::Show : HelperCall::Hide);
      if (value) {
        retirement.ever_shown = true;
      }
      retirement.hidden_verified = false;
      active = value;
      if (std::exchange(fail_write_once, false)) {
        return std::unexpected{AppError{.code = AppErrorCode::PlatformOperationFailure,
                                        .native_detail = 42}};
      }
      if (!value && readable) {
        retirement.hidden_verified = true;
        retirement.last_hide = native_completion;
      }
      return {};
    }

    [[nodiscard]] bool matches_active(bool value) noexcept
    {
      record(HelperCall::Verify);
      if (std::exchange(fail_verify_once, false) || fail_verify_always) {
        retirement.hidden_verified = false;
        return false;
      }
      return readable && active == value;
    }

    [[nodiscard]] unsigned resource_count() const noexcept
    {
      return static_cast<unsigned>(window) + static_cast<unsigned>(window_class);
    }

    [[nodiscard]] bool destroy_window() noexcept
    {
      if (!window) {
        return true;
      }
      record(HelperCall::Destroy);
      if (fail_destroy) {
        return false;
      }
      window = false;
      return true;
    }

    [[nodiscard]] bool unregister_class() noexcept
    {
      if (!window_class) {
        return true;
      }
      record(HelperCall::Unregister);
      if (fail_unregister) {
        return false;
      }
      window_class = false;
      return true;
    }
  };

}    // namespace

TEST_CASE(
  "avb0795 V1 case 11: Win32 overlay policy and transactional cleanup",
  "[avb0795-v1][windows][overlay]")
{
  SECTION("controller validation preserves screenshot visibility")
  {
    // A hidden layered top-level fixture supports affinity queries without
    // collecting pixels or presenting a test window on the user's desktop.
    const auto destroy_window = [](HWND window) noexcept {
      static_cast<void>(DestroyWindow(window));
    };
    const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(destroy_window)>
      window{CreateWindowExW(WS_EX_LAYERED, L"STATIC", L"", WS_POPUP,
                             0, 0, 16, 16, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr),
             destroy_window};
    REQUIRE(window != nullptr);
    REQUIRE(SetLayeredWindowAttributes(window.get(), 0, 255, LWA_ALPHA) != FALSE);
    DWORD affinity{};
    // DWM-backed affinity queries are available on an interactive desktop.
    // The returned setup status remains testable on headless Windows runners.
    const bool affinity_readable =
      GetWindowDisplayAffinity(window.get(), &affinity) != FALSE;
    if (affinity_readable) {
      REQUIRE(affinity == WDA_NONE);
    }

    const auto prepared = overlay::prepare_controller_window(window.get());
    REQUIRE(prepared.has_value());
    CHECK(*prepared == avb::core::CaptureExclusionStatus::NotAttempted);
    if (affinity_readable) {
      REQUIRE(GetWindowDisplayAffinity(window.get(), &affinity) != FALSE);
      CHECK(affinity == WDA_NONE);
    }
    else {
      WARN("DWM affinity query unavailable; controller status was verified");
    }

    const auto invalid = overlay::prepare_controller_window(nullptr);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == AppErrorCode::RuntimeConstructionFailure);
  }

  SECTION("controller native frame preserves client input and all resize edges")
  {
    const auto destroy_window = [](HWND window) noexcept {
      static_cast<void>(DestroyWindow(window));
    };
    const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(destroy_window)>
      window{CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
                             -800, -600, 640, 480, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr),
             destroy_window};
    REQUIRE(window != nullptr);
    const auto original_procedure = GetWindowLongPtrW(window.get(), GWLP_WNDPROC);
    auto adapter = overlay::WindowsControllerWindow::create(window.get());
    REQUIRE(adapter.has_value());
    REQUIRE_FALSE(IsWindowVisible(window.get()));
    const auto style = GetWindowLongPtrW(window.get(), GWL_STYLE);
    CHECK((style & (WS_CAPTION | WS_MAXIMIZEBOX)) == 0);
    CHECK((style & (WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX)) ==
          (WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX));
    RECT client{};
    RECT outer{};
    REQUIRE(GetClientRect(window.get(), &client) != FALSE);
    REQUIRE(GetWindowRect(window.get(), &outer) != FALSE);
    const auto width = client.right;
    const auto height = client.bottom;
    CHECK(width == outer.right - outer.left);
    CHECK(height == outer.bottom - outer.top);
    REQUIRE(outer.left < 0);
    REQUIRE(outer.top < 0);
    const auto hit_at = [&](LONG x, LONG y) {
      POINT screen{x, y};
      REQUIRE(ClientToScreen(window.get(), &screen) != FALSE);
      return SendMessageW(window.get(), WM_NCHITTEST, 0,
                          MAKELPARAM(static_cast<WORD>(screen.x),
                                     static_cast<WORD>(screen.y)));
    };
    (*adapter)->set_drag_region(0.75F, 0.25F);
    CHECK(hit_at(width / 2, height / 8) == HTCAPTION);
    CHECK(hit_at(width * 7 / 8, height / 8) == HTCLIENT);
    CHECK(hit_at(width / 2, height / 2) == HTCLIENT);
    struct EdgeProbe final {
      LONG x;
      LONG y;
      LRESULT expected;
    };
    const std::array edges{
      EdgeProbe{1, 1, HTTOPLEFT},
      EdgeProbe{width / 2, 1, HTTOP},
      EdgeProbe{width - 1, 1, HTTOPRIGHT},
      EdgeProbe{1, height / 2, HTLEFT},
      EdgeProbe{width - 1, height / 2, HTRIGHT},
      EdgeProbe{1, height - 1, HTBOTTOMLEFT},
      EdgeProbe{width / 2, height - 1, HTBOTTOM},
      EdgeProbe{width - 1, height - 1, HTBOTTOMRIGHT},
    };
    for (const auto &probe : edges) {
      CAPTURE(probe.x, probe.y);
      CHECK(hit_at(probe.x, probe.y) == probe.expected);
    }
    const auto dpi = GetDpiForWindow(window.get());
    REQUIRE(dpi != 0);
    const auto band = MulDiv(8, static_cast<int>(dpi), 96);
    CHECK(hit_at(band - 1, height / 2) == HTLEFT);
    CHECK(hit_at(band, height / 2) == HTCLIENT);
    MINMAXINFO minimum{};
    SendMessageW(window.get(), WM_GETMINMAXINFO, 0,
                 reinterpret_cast<LPARAM>(&minimum));
    CHECK(minimum.ptMinTrackSize.x == MulDiv(320, static_cast<int>(dpi), 96));
    CHECK(minimum.ptMinTrackSize.y == MulDiv(240, static_cast<int>(dpi), 96));
    NCCALCSIZE_PARAMS calculation{};
    calculation.rgrc[0] = outer;
    CHECK(SendMessageW(window.get(), WM_NCCALCSIZE, TRUE,
                       reinterpret_cast<LPARAM>(&calculation)) == 0);
    CHECK(EqualRect(&calculation.rgrc[0], &outer));
    auto simple_calculation = outer;
    CHECK(SendMessageW(window.get(), WM_NCCALCSIZE, FALSE,
                       reinterpret_cast<LPARAM>(&simple_calculation)) == 0);
    CHECK(EqualRect(&simple_calculation, &outer));
    CHECK(SendMessageW(window.get(), WM_NCLBUTTONDBLCLK, HTCAPTION, 0) == 0);
    CHECK_FALSE(IsZoomed(window.get()));
    (*adapter)->set_drag_region(0.0F, 0.0F);
    CHECK(hit_at(width / 2, height / 8) == HTCLIENT);
    CHECK(hit_at(1, 1) == HTTOPLEFT);
    (*adapter)->set_drag_region(0.75F, 0.25F);
    (*adapter)->set_drag_region((std::numeric_limits<float>::quiet_NaN)(), 0.25F);
    CHECK(hit_at(width / 2, height / 8) == HTCLIENT);
    // Programmatic sizing can bypass minimum tracking; opposing bands must not
    // overlap even when the caller supplies a tiny client extent.
    REQUIRE(SetWindowPos(window.get(), nullptr, 0, 0, 9, 9,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE |
                            SWP_NOSENDCHANGING) != FALSE);
    REQUIRE(GetClientRect(window.get(), &client) != FALSE);
    REQUIRE(client.right == 9);
    REQUIRE(client.bottom == 9);
    CHECK(hit_at(4, 4) == HTCLIENT);
    CHECK(hit_at(6, 1) == HTTOPRIGHT);
    CHECK(hit_at(1, 6) == HTBOTTOMLEFT);
    const auto duplicate = overlay::WindowsControllerWindow::create(window.get());
    CHECK_FALSE(duplicate.has_value());
    adapter->reset();
    CHECK(IsWindow(window.get()));
    CHECK(GetWindowLongPtrW(window.get(), GWLP_WNDPROC) == original_procedure);
    REQUIRE(overlay::WindowsControllerWindow::create(window.get()).has_value());
  }

  SECTION("controller invalid handles and native destruction detach safely")
  {
    const auto invalid = overlay::WindowsControllerWindow::create(nullptr);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == AppErrorCode::RuntimeConstructionFailure);
    const auto destroy_window = [](HWND window) noexcept {
      static_cast<void>(DestroyWindow(window));
    };
    const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(destroy_window)>
      window{CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
                             0, 0, 640, 480, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr),
             destroy_window};
    REQUIRE(window != nullptr);
    auto adapter = overlay::WindowsControllerWindow::create(window.get());
    REQUIRE(adapter.has_value());
    REQUIRE(DestroyWindow(window.get()) != FALSE);
    CHECK_FALSE(IsWindow(window.get()));
    (*adapter)->set_drag_region(0.75F, 0.25F);
    adapter->reset();
    CHECK_FALSE(overlay::WindowsControllerWindow::create(window.get()).has_value());
  }

  SECTION("browser helper eligibility and hidden preparation")
  {
    const helper::HelperActivationFacts eligible{true, true, true, true, false};
    CHECK(helper::helper_should_be_active(eligible));
    auto facts = eligible;
    facts.requested = false;
    CHECK_FALSE(helper::helper_should_be_active(facts));
    facts = eligible;
    facts.window_capture = false;
    CHECK_FALSE(helper::helper_should_be_active(facts));
    facts = eligible;
    facts.published_tick_ready = false;
    CHECK_FALSE(helper::helper_should_be_active(facts));
    facts = eligible;
    facts.source_connected = false;
    CHECK_FALSE(helper::helper_should_be_active(facts));
    facts = eligible;
    facts.stopping_or_terminal = true;
    CHECK_FALSE(helper::helper_should_be_active(facts));

    RecordingHelperOperations absent;
    REQUIRE(helper::set_helper_active(absent, false));
    CHECK(absent.resource_count() == 0);
    CHECK(absent.count(HelperCall::Prepare) == 0);
    RecordingHelperOperations failed{.fail_prepare = true};
    const auto result = helper::set_helper_active(failed, true);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == AppErrorCode::PlatformOperationFailure);
    CHECK(failed.count(HelperCall::Show) == 0);
    RecordingHelperOperations unreadable{.readable = false};
    REQUIRE_FALSE(helper::set_helper_active(unreadable, true));
    CHECK(unreadable.count(HelperCall::Prepare) == 0);
  }

  SECTION("browser helper repeated visibility and reversible rollback")
  {
    RecordingHelperOperations operations;
    REQUIRE(helper::set_helper_active(operations, true));
    REQUIRE(helper::set_helper_active(operations, true));
    CHECK(operations.active);
    CHECK(operations.count(HelperCall::Create) == 1);
    CHECK(operations.count(HelperCall::Show) == 1);
    CHECK(operations.count(HelperCall::Hide) == 0);
    operations.fail_verify_once = true;
    const auto disabled = helper::set_helper_active(operations, false);
    REQUIRE_FALSE(disabled);
    CHECK(disabled.error().code == AppErrorCode::PlatformOperationFailure);
    CHECK(operations.active);
    CHECK(operations.count(HelperCall::Show) == 2);

    RecordingHelperOperations lost;
    REQUIRE(helper::set_helper_active(lost, true));
    lost.fail_verify_always = true;
    const auto result = helper::set_helper_active(lost, false);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == AppErrorCode::RuntimeStateLost);
    CHECK(lost.active);
    CHECK(lost.count(HelperCall::Show) == 2);
    CHECK(lost.resource_count() == 2);
  }

  SECTION("browser helper live off and source loss retain ownership")
  {
    RecordingHelperOperations operations;
    REQUIRE(helper::set_helper_active(operations, true));
    REQUIRE(helper::set_helper_active(operations, false));
    CHECK_FALSE(operations.active);
    CHECK(operations.resource_count() == 2);
    CHECK(operations.count(HelperCall::Destroy) == 0);

    REQUIRE(helper::set_helper_active(operations, true));
    const auto shows = operations.count(HelperCall::Show);
    const auto prepares = operations.count(HelperCall::Prepare);
    operations.fail_verify_once = true;
    const auto lost = helper::deactivate_helper(operations);
    REQUIRE_FALSE(lost);
    CHECK(lost.error().code == AppErrorCode::RuntimeStateLost);
    CHECK_FALSE(operations.active);
    CHECK(operations.resource_count() == 2);
    REQUIRE(helper::deactivate_helper(operations));
    CHECK(operations.retirement.hidden_verified);
    CHECK(operations.count(HelperCall::Show) == shows);
    CHECK(operations.count(HelperCall::Prepare) == prepares);
    operations.fail_write_once = true;
    operations.retirement.hidden_verified = false;
    const auto failed_hide = helper::deactivate_helper(operations);
    REQUIRE_FALSE(failed_hide);
    CHECK(failed_hide.error().native_detail == 42);
  }

  SECTION("browser helper retirement uses completed hide and preserves grace")
  {
    using namespace std::chrono_literals;
    const auto step = helper::Clock::time_point{1s};
    const auto hidden = helper::Clock::time_point{1300ms};
    RecordingHelperOperations operations{.native_completion = hidden};
    operations.fail_verify_once = true;
    const auto show = helper::set_helper_active(operations, true);
    REQUIRE_FALSE(show);
    CHECK(show.error().code == AppErrorCode::PlatformOperationFailure);
    CHECK(operations.retirement.ever_shown);
    CHECK(operations.retirement.last_hide == hidden);
    operations.native_completion = hidden + 100ms;
    REQUIRE(helper::deactivate_helper(operations));
    CHECK(operations.retirement.last_hide == hidden);
    CHECK_FALSE(helper::progress_helper_retirement(operations, operations.retirement, step + 250ms).made_progress);
    CHECK_FALSE(helper::progress_helper_retirement(operations, operations.retirement, hidden + 249ms).made_progress);
    CHECK(operations.resource_count() == 2);
    CHECK(helper::helper_retirement_deadline(operations.retirement) == hidden + 250ms);
    auto uncertain = operations.retirement;
    uncertain.hidden_verified = false;
    CHECK_FALSE(helper::helper_may_retire(uncertain, hidden + 10s));
    uncertain.hidden_verified = true;
    uncertain.last_hide.reset();
    CHECK_FALSE(helper::helper_may_retire(uncertain, hidden + 10s));
    CHECK(helper::helper_may_retire({}, step));
    const auto retired = helper::progress_helper_retirement(operations, operations.retirement, hidden + 250ms);
    CHECK(retired.made_progress);
    CHECK_FALSE(retired.ownership_uncertain);
    CHECK(operations.resource_count() == 0);
    CHECK(operations.calls[operations.call_count - 2] == HelperCall::Destroy);
    CHECK(operations.calls[operations.call_count - 1] == HelperCall::Unregister);
  }

  SECTION("browser helper cleanup failures retain remaining ownership")
  {
    RecordingHelperOperations operations{.window = true, .window_class = true, .fail_destroy = true};
    const auto failed_destroy = helper::progress_helper_retirement(operations, operations.retirement, {});
    CHECK_FALSE(failed_destroy.made_progress);
    CHECK(failed_destroy.ownership_uncertain);
    CHECK(operations.resource_count() == 2);
    CHECK(operations.count(HelperCall::Unregister) == 0);
    operations.fail_destroy = false;
    operations.fail_unregister = true;
    const auto partial = helper::progress_helper_retirement(operations, operations.retirement, {});
    CHECK(partial.made_progress);
    CHECK(partial.ownership_uncertain);
    CHECK_FALSE(operations.window);
    CHECK(operations.window_class);
    const auto still_pending = helper::progress_helper_retirement(operations, operations.retirement, {});
    CHECK_FALSE(still_pending.made_progress);
    CHECK(still_pending.ownership_uncertain);
    operations.fail_unregister = false;
    CHECK(helper::progress_helper_retirement(operations, operations.retirement, {}).made_progress);
    CHECK(operations.resource_count() == 0);
    const auto empty = helper::progress_helper_retirement(operations, operations.retirement, {});
    CHECK_FALSE(empty.made_progress);
    CHECK_FALSE(empty.ownership_uncertain);
  }

  SECTION("browser helper and GPU retirement both gate runtime release")
  {
    using namespace std::chrono_literals;
    struct RetirementRow final {
      bool helper_releases_first{};
    };
    constexpr std::array rows{RetirementRow{true}, RetirementRow{false}};
    const auto hidden_at = helper::Clock::time_point{1s};
    const auto due = hidden_at + helper::helper_retirement_grace;

    for (const auto &row : rows) {
      CAPTURE(row.helper_releases_first);
      RecordingHelperOperations operations{
        .window = true,
        .window_class = true,
        .fail_destroy = true,
        .retirement = {.ever_shown = true,
                       .hidden_verified = true,
                       .last_hide = hidden_at},
      };
      bool helper_owner{true};
      bool gpu_pending{true};
      auto event_time = due;
      detail::RuntimeDrainState drain_state;
      const auto observe_drain = [&](const render::DrainProgress progress,
                                     const bool helper_uncertain,
                                     const auto now) {
        const auto update = detail::advance_runtime_drain(
          drain_state, helper_owner || gpu_pending, helper_uncertain, progress,
          now);
        drain_state = update.state;
        return update;
      };

      const auto retry = helper::progress_helper_retirement(
        operations, operations.retirement, due);
      CHECK_FALSE(retry.made_progress);
      CHECK(retry.ownership_uncertain);
      const auto retry_drain = observe_drain(
        render::DrainProgress::Retry, retry.ownership_uncertain, due);
      CHECK_FALSE(retry_drain.may_complete);
      CHECK(retry_drain.state.no_progress_since == due);

      operations.fail_destroy = false;
      const auto release_helper = [&] {
        event_time += 1ms;
        const auto retired = helper::progress_helper_retirement(
          operations, operations.retirement, event_time);
        REQUIRE(retired.made_progress);
        CHECK_FALSE(retired.ownership_uncertain);
        auto progress = render::DrainProgress::TrackedRetirement;
        if (operations.resource_count() == 0 && !retired.ownership_uncertain) {
          helper_owner = false;
          progress = render::DrainProgress::ProvenSafeRelease;
        }
        return observe_drain(progress, retired.ownership_uncertain, event_time);
      };
      const auto release_gpu = [&] {
        event_time += 1ms;
        gpu_pending = false;
        return observe_drain(render::DrainProgress::ProvenSafeRelease, false,
                             event_time);
      };

      const auto first =
        row.helper_releases_first ? release_helper() : release_gpu();
      CHECK_FALSE(first.may_complete);
      CHECK(first.state.ownership_uncertain);
      const auto final =
        row.helper_releases_first ? release_gpu() : release_helper();
      CHECK(final.may_complete);
      CHECK_FALSE(final.state.ownership_uncertain);
      CHECK_FALSE(helper_owner);
      CHECK_FALSE(gpu_pending);
    }
  }

  static_assert(!std::is_copy_constructible_v<overlay::OverlayWindow>);
  static_assert(!std::is_move_constructible_v<overlay::OverlayWindow>);
  static_assert(std::is_nothrow_destructible_v<overlay::OverlayWindow>);
  static_assert(
    noexcept(std::declval<const overlay::OverlayWindow &>().border_matches(
      BorderPreference::Bordered)));
  static_assert(
    std::is_same_v<decltype(std::declval<const overlay::OverlayWindow &>()
                              .border_matches(BorderPreference::Bordered)),
                   bool>);
  static_assert(!std::is_copy_constructible_v<overlay::Win32OverlayAdapter>);
  static_assert(!std::is_move_constructible_v<overlay::Win32OverlayAdapter>);

  SECTION("class style and exact input transparency decisions")
  {
    const auto oversized = overlay::OverlayWindow::create(
      {.width = (std::numeric_limits<std::uint32_t>::max)(), .height = 1},
      BorderPreference::Bordered);
    REQUIRE_FALSE(oversized.has_value());
    CHECK(oversized.error().code == AppErrorCode::InvalidValue);

    constexpr std::array rows{
      std::pair{std::uintptr_t{0}, true},
      std::pair{detail::class_style_own_dc_bit, false},
      std::pair{detail::class_style_class_dc_bit, false},
      std::pair{detail::class_style_own_dc_bit |
                  detail::class_style_class_dc_bit,
                false},
    };
    for (const auto &[style, expected] : rows) {
      CAPTURE(style);
      CHECK(detail::is_overlay_class_style_supported(style) == expected);
    }

    constexpr auto unrelated = std::uintptr_t{0x40000000};
    const auto pass_through =
      detail::input_transparency_style(unrelated, OverlayMode::PassThrough);
    CHECK(pass_through == (unrelated | detail::layered_style_bit |
                           detail::transparent_style_bit));
    CHECK(detail::input_transparency_style(pass_through,
                                           OverlayMode::Editable) == unrelated);
  }

  SECTION("editable hit zones and pass through")
  {
    const detail::NativeRect window{
      .left = 100, .top = 200, .right = 500, .bottom = 500};
    struct Row final {
      detail::NativePoint point;
      detail::HitZone expected;
    };
    constexpr std::array rows{
      Row{{102, 202}, detail::HitZone::TopLeft},
      Row{{300, 202}, detail::HitZone::Top},
      Row{{498, 202}, detail::HitZone::TopRight},
      Row{{102, 350}, detail::HitZone::Left},
      Row{{300, 350}, detail::HitZone::Caption},
      Row{{498, 350}, detail::HitZone::Right},
      Row{{102, 498}, detail::HitZone::BottomLeft},
      Row{{300, 498}, detail::HitZone::Bottom},
      Row{{498, 498}, detail::HitZone::BottomRight},
    };
    for (const auto &row : rows) {
      CAPTURE(row.point.x, row.point.y);
      CHECK(detail::overlay_hit_test(OverlayMode::Editable,
                                     BorderPreference::Borderless, window,
                                     row.point, 8) == row.expected);
    }
    CHECK(detail::overlay_hit_test(OverlayMode::Editable,
                                   BorderPreference::Bordered, window,
                                   {300, 350}, 8) == detail::HitZone::Delegate);
    CHECK(detail::overlay_hit_test(
            OverlayMode::PassThrough, BorderPreference::Bordered, window,
            {300, 350}, 8) == detail::HitZone::Transparent);
  }

  SECTION("opaque layered setup and exact state restoration")
  {
    constexpr auto base_style = std::uintptr_t{0x04000000};
    RecordingNativeWindowOperations operations;
    operations.state = {.extended_style = base_style};
    const auto entered = detail::set_native_presentation(operations, OverlayMode::PassThrough, 100);
    REQUIRE(entered.has_value());
    CHECK(operations.state.extended_style ==
          (base_style | detail::layered_style_bit |
           detail::transparent_style_bit));
    REQUIRE(operations.state.layered_attributes.has_value());
    CHECK(*operations.state.layered_attributes ==
          detail::opaque_layered_attributes);
    REQUIRE(operations.calls.size() >= 4);
    CHECK(operations.calls[1].call == NativeCall::WriteStyle);
    CHECK(operations.calls[2].call == NativeCall::WriteLayered);
    CHECK(operations.calls[2].layered == detail::opaque_layered_attributes);

    REQUIRE(detail::set_native_presentation(operations, OverlayMode::Editable, 100).has_value());
    CHECK(operations.state == detail::NativeWindowState{.extended_style = base_style});

    const detail::LayeredAttributes original_layered{
      .color_key = 0x00010203,
      .alpha = 117,
      .flags = detail::layered_alpha_flag,
    };
    RecordingNativeWindowOperations layered_operations;
    layered_operations.state = {.extended_style =
                                  base_style | detail::layered_style_bit,
                                .layered_attributes = original_layered};
    const auto layered_entered = detail::set_native_presentation(layered_operations, OverlayMode::PassThrough, 46);
    REQUIRE(layered_entered.has_value());
    REQUIRE(detail::set_native_presentation(layered_operations, OverlayMode::Editable, 46).has_value());
    CHECK(layered_operations.state.extended_style == (base_style | detail::layered_style_bit));
    CHECK(layered_operations.state.layered_attributes ==
          detail::LayeredAttributes{.color_key = 0, .alpha = 117, .flags = detail::layered_alpha_flag});
    for (const int percent : {-1, 101}) {
      const auto before = layered_operations.state;
      const auto rejected = detail::set_native_presentation(layered_operations, OverlayMode::Editable, percent);
      REQUIRE_FALSE(rejected.has_value());
      CHECK(rejected.error().code == AppErrorCode::InvalidValue);
      CHECK(layered_operations.state == before);
    }

    RecordingNativeWindowOperations unreadable;
    unreadable.state = {.extended_style =
                          base_style | detail::layered_style_bit,
                        .layered_attributes = original_layered};
    unreadable.layered_state_readable = false;
    const auto rejected = detail::set_native_presentation(unreadable, OverlayMode::PassThrough, 50);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == AppErrorCode::PlatformOperationFailure);
    CHECK(unreadable.calls.size() == 2);
  }

  SECTION("native hidden overlay preserves border when returning from pass through")
  {
    const TestGlfwLifetime glfw;
    REQUIRE(glfw.initialized());
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    constexpr LONG_PTR input_bits = WS_EX_TRANSPARENT | WS_EX_LAYERED;
    constexpr Extent extent{.width = 320, .height = 240};
    constexpr std::array initial_borders{
      BorderPreference::Borderless, BorderPreference::Bordered};
    for (const auto initial_border : initial_borders) {
      CAPTURE(initial_border);
      auto window = overlay::OverlayWindow::create(extent, initial_border);
      REQUIRE(window.has_value());
      const auto hwnd = find_test_overlay_window();
      REQUIRE(hwnd != nullptr);
      REQUIRE_FALSE(IsWindowVisible(hwnd));
      const auto native_long = [&](const int index) {
        SetLastError(ERROR_SUCCESS);
        const auto value = GetWindowLongPtrW(hwnd, index);
        REQUIRE((value != 0 || GetLastError() == ERROR_SUCCESS));
        return value;
      };
      const auto original_procedure = native_long(GWLP_WNDPROC);
      const auto initial_style = native_long(GWL_STYLE);
      const auto initial_extended_style = native_long(GWL_EXSTYLE);
      REQUIRE((initial_extended_style & input_bits) == 0);
      auto detached = overlay::Win32OverlayAdapter::create_detached(
        **window, OverlayMode::Editable, initial_border, extent);
      REQUIRE(detached.has_value());
      // Install cleanup before attach: a failing assertion or partial attach can
      // require HWND destruction while its callback context is still alive.
      const auto retire_adapter = [&](overlay::Win32OverlayAdapter *adapter) noexcept {
        std::unique_ptr<overlay::Win32OverlayAdapter> owner{adapter};
        if (owner->window_destruction_required() || !owner->detach()) {
          (*window)->destroy();
          owner->complete_after_overlay_destroyed();
        }
      };
      const std::unique_ptr<overlay::Win32OverlayAdapter, decltype(retire_adapter)>
        adapter{detached->release(), retire_adapter};
      REQUIRE(adapter->attach().has_value());
      const auto check_state = [&](const OverlayMode mode,
                                   const BorderPreference border) {
        const auto style = native_long(GWL_STYLE);
        const auto extended_style = native_long(GWL_EXSTYLE);
        CAPTURE(mode, border, style, extended_style, adapter->mode());
        CHECK(adapter->mode() == mode);
        CHECK(adapter->border_preference() == border);
        CHECK((*window)->border() == border);
        CHECK((*window)->border_matches(border));
        CHECK((style & WS_CAPTION) ==
              (border == BorderPreference::Bordered ? WS_CAPTION : 0));
        CHECK((extended_style & input_bits) ==
              (mode == OverlayMode::PassThrough ? input_bits : 0));
        CHECK_FALSE(IsWindowVisible(hwnd));
        RECT client{};
        REQUIRE(GetClientRect(hwnd, &client) != FALSE);
        POINT center{.x = (client.left + client.right) / 2,
                     .y = (client.top + client.bottom) / 2};
        REQUIRE(ClientToScreen(hwnd, &center) != FALSE);
        const auto hit = SendMessageW(
          hwnd, WM_NCHITTEST, 0,
          MAKELPARAM(static_cast<WORD>(center.x), static_cast<WORD>(center.y)));
        CHECK(hit == (mode == OverlayMode::PassThrough ? HTTRANSPARENT :
                      border == BorderPreference::Bordered ? HTCLIENT : HTCAPTION));
      };
      check_state(OverlayMode::Editable, initial_border);
      REQUIRE(adapter->set_mode(OverlayMode::PassThrough).has_value());
      check_state(OverlayMode::PassThrough, initial_border);
      CHECK(native_long(GWL_STYLE) == initial_style);
      const auto changed_border = initial_border == BorderPreference::Bordered
                                    ? BorderPreference::Borderless
                                    : BorderPreference::Bordered;
      REQUIRE((*window)->set_border(changed_border).has_value());
      adapter->set_border_preference(changed_border);
      check_state(OverlayMode::PassThrough, changed_border);
      const auto changed_style = native_long(GWL_STYLE);
      const auto changed_extended_style = native_long(GWL_EXSTYLE);
      const auto left = adapter->set_mode(OverlayMode::Editable);
      CAPTURE(initial_style, initial_extended_style, changed_style,
              changed_extended_style, native_long(GWL_STYLE),
              native_long(GWL_EXSTYLE));
      if (!left) {
        CAPTURE(left.error().code, left.error().native_detail);
        CHECK(left.has_value());
      }
      check_state(OverlayMode::Editable, changed_border);
      CHECK(native_long(GWL_STYLE) == changed_style);
      CHECK((native_long(GWL_EXSTYLE) & ~input_bits) ==
            (changed_extended_style & ~input_bits));
      const auto check_opacity = [&](overlay::Win32OverlayAdapter &native_adapter) {
        for (const auto mode : {OverlayMode::Editable, OverlayMode::PassThrough}) {
          for (const auto percent : {0, 1, 50, 99, 100}) {
            CAPTURE(mode, percent, changed_border);
            REQUIRE(native_adapter.set_presentation(mode, percent).has_value());
            CHECK(native_adapter.mode() == mode);
            CHECK(native_adapter.opacity_percent() == percent);
            CHECK((*window)->border_matches(changed_border));
            const auto exstyle = native_long(GWL_EXSTYLE);
            CHECK((exstyle & ~input_bits) ==
                  (changed_extended_style & ~input_bits));
            CHECK(((exstyle & WS_EX_TRANSPARENT) != 0) ==
                  (mode == OverlayMode::PassThrough));
            CHECK(((exstyle & WS_EX_LAYERED) != 0) ==
                  (mode == OverlayMode::PassThrough || percent < 100));
            if ((exstyle & WS_EX_LAYERED) != 0) {
              COLORREF color{};
              BYTE alpha{};
              DWORD flags{};
              REQUIRE(GetLayeredWindowAttributes(hwnd, &color, &alpha, &flags));
              CHECK(alpha == (percent * 255 + 50) / 100);
              CHECK(flags == LWA_ALPHA);
            }
          }
        }
        REQUIRE(native_adapter.set_presentation(OverlayMode::Editable, 37).has_value());
        REQUIRE(native_adapter.set_mode(OverlayMode::PassThrough).has_value());
        CHECK(native_adapter.opacity_percent() == 37);
        REQUIRE_FALSE(native_adapter.set_presentation(OverlayMode::Editable, -1).has_value());
        CHECK(native_adapter.mode() == OverlayMode::PassThrough);
        CHECK(native_adapter.opacity_percent() == 37);
      };
      check_opacity(*adapter);
      REQUIRE(adapter->detach().has_value());
      CHECK_FALSE(adapter->attached());
      CHECK_FALSE(adapter->window_destruction_required());
      CHECK(native_long(GWLP_WNDPROC) == original_procedure);
      (*window)->destroy();
      CHECK_FALSE(IsWindow(hwnd));
    }
  }

  SECTION("native region waits for content and completes a bounded captured drag")
  {
    with_region_overlay([](const HWND hwnd, overlay::Win32OverlayAdapter &adapter) {
      constexpr detail::NativeRect content{20, 40, 300, 200};
      const auto pointer = [&](const UINT message, const int x, const int y) {
        return SendMessageW(hwnd, message, message == WM_LBUTTONUP ? 0 : MK_LBUTTON,
                            MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y)));
      };
      adapter.set_region_selection(true);
      pointer(WM_LBUTTONDOWN, 100, 100);
      CHECK(GetCapture() != hwnd);
      CHECK_FALSE(adapter.region_selection_preview());
      SendMessageW(hwnd, WM_KILLFOCUS, 0, 0); // no focus was acquired while pending
      CHECK_FALSE(adapter.take_region_selection_result());
      adapter.set_region_selection(true, content);
      POINT center{160, 120};
      REQUIRE(ClientToScreen(hwnd, &center));
      CHECK(SendMessageW(hwnd, WM_NCHITTEST, 0,
                         MAKELPARAM(static_cast<WORD>(center.x),
                                    static_cast<WORD>(center.y))) == HTCLIENT);
      CHECK(SendMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd),
                         MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)) == TRUE);
      CHECK(GetCursor() == LoadCursor(nullptr, IDC_CROSS));
      pointer(WM_LBUTTONDOWN, 10, 100); // letterbox never starts a gesture
      CHECK(GetCapture() != hwnd);
      pointer(WM_LBUTTONDOWN, 100, 100);
      REQUIRE(GetCapture() == hwnd); // SetCapture returns the previous owner
      pointer(WM_LBUTTONUP, 100, 100); // a point releases capture and permits retry
      CHECK(GetCapture() != hwnd);
      CHECK_FALSE(adapter.take_region_selection_result());
      CHECK_FALSE(adapter.region_selection_preview());
      pointer(WM_LBUTTONDOWN, 100, 100);
      pointer(WM_LBUTTONUP, 100, 160); // a line also has zero area
      CHECK(GetCapture() != hwnd);
      CHECK_FALSE(adapter.take_region_selection_result());
      pointer(WM_LBUTTONDOWN, 250, 170);
      REQUIRE(GetCapture() == hwnd);
      adapter.set_region_selection(true, content); // unchanged mapping preserves capture
      CHECK(GetCapture() == hwnd);
      pointer(WM_MOUSEMOVE, -20, 400);
      REQUIRE(adapter.region_selection_preview());
      CHECK(*adapter.region_selection_preview() ==
            overlay::OverlayRegionGesture{{250, 170}, {20, 200}});
      pointer(WM_LBUTTONUP, -20, 400);
      CHECK(GetCapture() != hwnd);
      const auto result = adapter.take_region_selection_result();
      REQUIRE(result);
      REQUIRE(result->gesture);
      CHECK(*result->gesture == overlay::OverlayRegionGesture{{250, 170}, {20, 200}});
      CHECK_FALSE(adapter.take_region_selection_result());
      adapter.set_region_selection(true, content);
      pointer(WM_LBUTTONDOWN, 100, 100); // terminal remains frozen even after take
      CHECK(GetCapture() != hwnd);
      CHECK_FALSE(adapter.region_selection_preview());
      adapter.set_region_selection(false);
      CHECK(SendMessageW(hwnd, WM_NCHITTEST, 0,
                         MAKELPARAM(static_cast<WORD>(center.x),
                                    static_cast<WORD>(center.y))) == HTCAPTION);
    });
  }

  SECTION("native region cancellation and detach clear state before capture reentry")
  {
    with_region_overlay([](const HWND hwnd, overlay::Win32OverlayAdapter &adapter) {
      constexpr detail::NativeRect content{20, 40, 300, 200};
      for (const auto message : {WM_KEYDOWN, WM_KILLFOCUS, WM_CANCELMODE,
                                 WM_CAPTURECHANGED}) {
        CAPTURE(message);
        adapter.set_region_selection(false);
        adapter.set_region_selection(true, content);
        SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
        REQUIRE(GetCapture() == hwnd);
        if (message == WM_CAPTURECHANGED) {
          REQUIRE(ReleaseCapture()); // actual nested native notification
        }
        else {
          SendMessageW(hwnd, message, message == WM_KEYDOWN ? VK_ESCAPE : 0, 0);
        }
        CHECK(GetCapture() != hwnd);
        CHECK_FALSE(adapter.region_selection_preview());
        const auto result = adapter.take_region_selection_result();
        REQUIRE(result);
        CHECK_FALSE(result->gesture);
        CHECK_FALSE(adapter.take_region_selection_result());
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
        CHECK(GetCapture() != hwnd);
      }
      adapter.set_region_selection(false);
      adapter.set_region_selection(true, content);
      SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
      REQUIRE(GetCapture() == hwnd);
      adapter.set_region_selection(false);
      CHECK(GetCapture() != hwnd);
      CHECK_FALSE(adapter.take_region_selection_result());
      adapter.set_region_selection(true, content);
      SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
      REQUIRE(GetCapture() == hwnd);
      adapter.set_region_selection(true, detail::NativeRect{20, 50, 300, 200});
      CHECK(GetCapture() != hwnd);
      const auto moved_content = adapter.take_region_selection_result();
      REQUIRE(moved_content);
      CHECK_FALSE(moved_content->gesture);
      adapter.set_region_selection(false);
      adapter.set_region_selection(true);
      EnableWindow(hwnd, FALSE); // SetFocus must fail on a disabled window
      CHECK_FALSE(adapter.focus_region_selection());
      const auto failed_focus = adapter.take_region_selection_result();
      REQUIRE(failed_focus);
      CHECK_FALSE(failed_focus->gesture);
      EnableWindow(hwnd, TRUE);
      adapter.set_region_selection(false);
      adapter.set_region_selection(true, content);
      SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
      REQUIRE(GetCapture() == hwnd);
      REQUIRE(adapter.detach());
      CHECK(GetCapture() != hwnd);
      CHECK_FALSE(adapter.region_selection_preview());
      CHECK_FALSE(adapter.take_region_selection_result());
    });
  }

  SECTION("leaving pass through preserves independent style edits and rollback")
  {
    constexpr auto retained = std::uintptr_t{WS_EX_NOACTIVATE};
    constexpr auto removed = std::uintptr_t{WS_EX_APPWINDOW};
    constexpr auto added = std::uintptr_t{WS_EX_TOOLWINDOW};
    constexpr detail::LayeredAttributes original_layered{
      .color_key = 0x00010203, .alpha = 117, .flags = LWA_ALPHA | LWA_COLORKEY};
    for (const bool initially_layered : {false, true}) {
      CAPTURE(initially_layered);
      RecordingNativeWindowOperations operations;
      operations.state = {
        .extended_style = retained | removed |
                          (initially_layered ? detail::layered_style_bit : 0),
        .layered_attributes = initially_layered
                                ? std::optional{original_layered}
                                : std::nullopt,
      };
      const auto entered = detail::set_native_presentation(operations, OverlayMode::PassThrough, 50);
      REQUIRE(entered.has_value());
      // Independent border policy changes must survive presentation changes.
      operations.state.extended_style =
        (operations.state.extended_style & ~removed) | added;
      const auto before_leave = operations.state;
      operations.fail_once = initially_layered ? NativeCall::WriteLayered
                                               : NativeCall::WriteStyle;
      const int percent = initially_layered ? 37 : 100;
      const auto failed = detail::set_native_presentation(operations, OverlayMode::Editable, percent);
      REQUIRE_FALSE(failed.has_value());
      CHECK(failed.error().code == AppErrorCode::PlatformOperationFailure);
      CHECK(operations.state == before_leave);
      REQUIRE(detail::set_native_presentation(operations, OverlayMode::Editable, percent).has_value());
      const detail::NativeWindowState expected{
        .extended_style = retained | added |
                          (initially_layered ? detail::layered_style_bit : 0),
        .layered_attributes = initially_layered
          ? std::optional{detail::LayeredAttributes{.color_key = 0, .alpha = 94, .flags = LWA_ALPHA}}
          : std::nullopt,
      };
      CHECK(operations.state == expected);
    }

    RecordingNativeWindowOperations lost;
    lost.state = {
      .extended_style = retained | added | detail::layered_style_bit |
                        detail::transparent_style_bit,
      .layered_attributes = detail::opaque_layered_attributes,
    };
    lost.fail_once = NativeCall::WriteLayered;
    lost.corrupt_rollback_verification = true;
    const auto failed = detail::set_native_presentation(lost, OverlayMode::Editable, 37);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == AppErrorCode::RuntimeStateLost);
  }

  SECTION("alpha failure rolls back and verification loss is terminal")
  {
    constexpr auto base_style = std::uintptr_t{0x01000000};
    RecordingNativeWindowOperations recoverable;
    recoverable.state = {.extended_style = base_style};
    recoverable.fail_once = NativeCall::WriteLayered;
    const auto recoverable_result = detail::set_native_presentation(recoverable, OverlayMode::PassThrough, 50);
    REQUIRE_FALSE(recoverable_result.has_value());
    CHECK(recoverable_result.error().code ==
          AppErrorCode::PlatformOperationFailure);
    CHECK(recoverable.state ==
          detail::NativeWindowState{.extended_style = base_style});

    RecordingNativeWindowOperations lost;
    lost.state = {.extended_style = base_style};
    lost.fail_once = NativeCall::WriteLayered;
    lost.corrupt_rollback_verification = true;
    const auto lost_result = detail::set_native_presentation(lost, OverlayMode::PassThrough, 50);
    REQUIRE_FALSE(lost_result.has_value());
    CHECK(lost_result.error().code == AppErrorCode::RuntimeStateLost);
  }

  SECTION("content fit preserves client width within work area")
  {
    constexpr detail::NativeRect client{500, 400, 1140, 760};
    constexpr detail::NativeRect work{0, 0, 2560, 1440};
    struct Example final {
      Extent source;
      detail::FrameInsets insets;
      int width;
      int height;
    };
    constexpr std::array examples{
      Example{{4, 3}, {}, 640, 480},
      Example{{16, 9}, {}, 640, 360},
      Example{{1, 2}, {}, 640, 1280},
      Example{{1, 10}, {}, 144, 1440},
      Example{{4, 3}, {8, 31, 8, 8}, 640, 480},
      Example{{1, 10}, {8, 31, 8, 8}, 140, 1401},
      Example{{1000000, 1}, {}, 640, 1},
      Example{{1, 1000000}, {}, 1, 1440},
    };
    for (const auto &example : examples) {
      CAPTURE(example.source.width, example.source.height, example.insets.top);
      const auto result = detail::fit_content_rect(client, example.source, example.insets, work);
      REQUIRE(result);
      const auto content = detail::outer_to_content_rect(*result, example.insets);
      CHECK(content.right - content.left == example.width);
      CHECK(content.bottom - content.top == example.height);
      CHECK(result->left >= work.left);
      CHECK(result->top >= work.top);
      CHECK(result->right <= work.right);
      CHECK(result->bottom <= work.bottom);
      if (example.height <= 480) {
        CHECK(std::abs((content.left + content.right) - (client.left + client.right)) <= 1);
        CHECK(std::abs((content.top + content.bottom) - (client.top + client.bottom)) <= 1);
      }
    }
    const auto tiny = detail::fit_content_rect({10, 20, 11, 21}, {16, 9}, {}, work);
    REQUIRE(tiny);
    CHECK(tiny->right - tiny->left == 1);
    CHECK(tiny->bottom - tiny->top == 1);
    const auto rounded = detail::fit_content_rect({100, 100, 201, 201}, {16, 9}, {}, work);
    REQUIRE(rounded);
    CHECK(rounded->right - rounded->left == 101);
    CHECK(rounded->bottom - rounded->top == 57);
    const auto shifted = detail::fit_content_rect({-100, -100, 540, 260}, {4, 3}, {}, work);
    REQUIRE(shifted);
    CHECK(*shifted == detail::NativeRect{0, 0, 640, 480});
    CHECK_FALSE(detail::fit_content_rect(client, {}, {}, work));
    CHECK_FALSE(detail::fit_content_rect(client, {16, 9}, {-1, 0, 0, 0}, work));
    CHECK_FALSE(detail::fit_content_rect(client, {16, 9}, {2000, 0, 2000, 0}, work));
    CHECK_FALSE(detail::fit_content_rect({}, {16, 9}, {}, work));
    CHECK_FALSE(detail::fit_content_rect({10, 10, 0, 0}, {16, 9}, {}, work));
    CHECK_FALSE(detail::fit_content_rect(client, {16, 9}, {}, {}));
    constexpr detail::NativeRect overflow{(std::numeric_limits<int>::min)(), 0,
                                         (std::numeric_limits<int>::max)(), 100};
    CHECK_FALSE(detail::fit_content_rect(overflow, {16, 9}, {}, work));
    CHECK_FALSE(detail::fit_content_rect(client, {16, 9}, {}, overflow));
    constexpr auto far_edge = (std::numeric_limits<std::int32_t>::max)();
    const auto far_monitor = detail::fit_content_rect(
      {far_edge - 800, far_edge - 800, far_edge - 160, far_edge - 440}, {4, 3}, {},
      {far_edge - 1920, far_edge - 1080, far_edge, far_edge});
    REQUIRE(far_monitor);
    CHECK(far_monitor->right - far_monitor->left == 640);
    CHECK(far_monitor->bottom - far_monitor->top == 480);
  }

  SECTION("native hidden content fit and exact restoration preserve geometry")
  {
    const TestGlfwLifetime glfw;
    REQUIRE(glfw.initialized());
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    const auto focus = GetFocus();
    const auto capture = GetCapture();
    for (const auto border : {BorderPreference::Borderless, BorderPreference::Bordered}) {
      CAPTURE(border);
      auto window = overlay::OverlayWindow::create({640, 360}, border);
      REQUIRE(window);
      const auto hwnd = find_test_overlay_window();
      REQUIRE(hwnd != nullptr);
      REQUIRE_FALSE(IsWindowVisible(hwnd));
      auto adapter = overlay::Win32OverlayAdapter::create_detached(
        **window, OverlayMode::PassThrough, border, {16, 9});
      REQUIRE(adapter);
      MONITORINFO monitor{.cbSize = sizeof(MONITORINFO)};
      REQUIRE(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor));
      const detail::NativeRect work{monitor.rcWork.left, monitor.rcWork.top,
                                     monitor.rcWork.right, monitor.rcWork.bottom};
      const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      RECT frame{0, 0, 1, 1};
      REQUIRE(AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(style), FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd)));
      const detail::FrameInsets insets{-frame.left, -frame.top, frame.right - 1, frame.bottom - 1};
      // Preserve an original snapshot outside the work area to exercise exact rollback.
      REQUIRE(SetWindowPos(hwnd, nullptr, work.left - 80, work.top - 60, 0, 0,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE));
      const auto original = (*adapter)->snapshot_client_geometry();
      REQUIRE(original);
      for (const auto source : {Extent{4, 3}, Extent{1, 100}}) {
        CAPTURE(source.width, source.height);
        const auto expected_outer = detail::fit_content_rect(original->client_screen_rect, source, insets, work);
        REQUIRE(expected_outer);
        const overlay::OverlayClientGeometry expected{
          detail::outer_to_content_rect(*expected_outer, insets)};
        const auto fitted = (*adapter)->fit_client_geometry(*original, source);
        const auto observed = (*adapter)->snapshot_client_geometry();
        REQUIRE(observed);
        CAPTURE(expected.client_screen_rect.left, expected.client_screen_rect.top,
          expected.client_screen_rect.right, expected.client_screen_rect.bottom,
          observed->client_screen_rect.left, observed->client_screen_rect.top,
          observed->client_screen_rect.right, observed->client_screen_rect.bottom);
        REQUIRE(fitted);
        CHECK(*fitted == expected);
        CHECK((*adapter)->client_geometry_matches(expected));
        CHECK(GetWindowLongPtrW(hwnd, GWL_STYLE) == style);
        REQUIRE((*adapter)->restore_client_geometry(*original, overlay::GeometryRestoreBounds::Exact));
        CHECK((*adapter)->client_geometry_matches(*original));
      }
      if (border == BorderPreference::Borderless) {
        REQUIRE(SetWindowPos(hwnd, nullptr, work.left + 100, work.top + 100, 1, 1,
          SWP_NOACTIVATE | SWP_NOZORDER));
        const auto tiny = (*adapter)->snapshot_client_geometry();
        REQUIRE(tiny);
        const auto fitted = (*adapter)->fit_client_geometry(*tiny, {16, 9});
        REQUIRE(fitted);
        CHECK(fitted->client_screen_rect.right - fitted->client_screen_rect.left == 1);
        CHECK(fitted->client_screen_rect.bottom - fitted->client_screen_rect.top == 1);
      }
      CHECK_FALSE((*adapter)->fit_client_geometry(*original, {}));
      const overlay::OverlayClientGeometry overflow{{(std::numeric_limits<int>::min)(), 0,
        (std::numeric_limits<int>::max)(), 100}};
      CHECK_FALSE((*adapter)->restore_client_geometry(overflow, overlay::GeometryRestoreBounds::Exact));
      if (border == BorderPreference::Bordered) {
        const overlay::OverlayClientGeometry saturated{{(std::numeric_limits<int>::min)(), 0,
          (std::numeric_limits<int>::min)() + 640, 360}};
        CHECK_FALSE((*adapter)->restore_client_geometry(saturated, overlay::GeometryRestoreBounds::Exact));
      }
      CHECK_FALSE(IsWindowVisible(hwnd));
      CHECK(GetFocus() == focus);
      CHECK(GetCapture() == capture);
    }
  }

  SECTION("wheel geometry preserves aspect within work area")
  {
    constexpr detail::NativeRect client{500, 400, 1140, 760};
    constexpr detail::NativeRect work{0, 0, 2560, 1440};
    struct Example final {
      Extent source;
      int notches;
      int percent;
      std::uint32_t dpi;
      detail::FrameInsets insets;
      int width;
      int height;
    };
    constexpr std::array examples{
      Example{{16, 9}, 1, 5, 96, {}, 672, 378},
      Example{{16, 9}, -1, 5, 96, {}, 610, 343},
      Example{{16, 9}, 2, 25, 96, {}, 1000, 563},
      Example{{16, 9}, -2, 25, 96, {}, 410, 230},
      Example{{4, 3}, 1, 5, 96, {8, 31, 8, 8}, 672, 504},
      Example{{1, 1}, -1000, 5, 144, {}, 240, 240},
      Example{{16, 9}, -1000, 5, 192, {}, 320, 180},
      Example{{16, 9}, (std::numeric_limits<int>::max)(), 25, 96, {}, 2560, 1440},
      Example{{16, 9}, (std::numeric_limits<int>::min)(), 25, 96, {}, 160, 90},
    };
    for (const auto &example : examples) {
      CAPTURE(example.notches, example.percent, example.dpi, example.source.width);
      const auto result = detail::wheel_resized_rect(client, example.source,
        example.insets, work, example.dpi, example.notches, example.percent);
      REQUIRE(result);
      const auto content = detail::outer_to_content_rect(*result, example.insets);
      CHECK(content.right - content.left == example.width);
      CHECK(content.bottom - content.top == example.height);
      CHECK(result->left >= work.left);
      CHECK(result->top >= work.top);
      CHECK(result->right <= work.right);
      CHECK(result->bottom <= work.bottom);
      if (example.notches > -100 && example.notches < 100) {
        CHECK(std::abs((content.left + content.right) - (client.left + client.right)) <= 1);
        CHECK(std::abs((content.top + content.bottom) - (client.top + client.bottom)) <= 1);
      }
    }
    constexpr detail::NativeRect negative_work{-1920, -1080, 0, 0};
    const auto bounded = detail::wheel_resized_rect({-400, -250, -80, -70},
      {16, 9}, {8, 31, 8, 8}, negative_work, 96, 1000, 25);
    REQUIRE(bounded);
    CHECK(bounded->left >= negative_work.left);
    CHECK(bounded->top >= negative_work.top);
    CHECK(bounded->right <= 0);
    CHECK(bounded->bottom <= 0);
    CHECK(bounded->right - bounded->left == 1867);
    CHECK(bounded->bottom - bounded->top == 1080);

    for (const auto source : std::array{Extent{16, 9}, Extent{1, 1000000},
                                       Extent{1000000, 1}}) {
      const auto tiny = detail::wheel_resized_rect(client, source, {},
        {-100, -50, 0, 0}, 192, -1000, 5);
      REQUIRE(tiny);
      CHECK(tiny->right - tiny->left >= 1);
      CHECK(tiny->bottom - tiny->top >= 1);
      CHECK(tiny->left >= -100);
      CHECK(tiny->top >= -50);
      CHECK(tiny->right <= 0);
      CHECK(tiny->bottom <= 0);
      CHECK((std::abs((tiny->bottom - tiny->top) -
        (tiny->right - tiny->left) * static_cast<double>(source.height) / source.width) <= 1.0 ||
        std::abs((tiny->right - tiny->left) -
        (tiny->bottom - tiny->top) * static_cast<double>(source.width) / source.height) <= 1.0));
    }
    for (const int step : {0, 26, (std::numeric_limits<int>::max)()}) {
      CHECK_FALSE(detail::wheel_resized_rect(client, {16, 9}, {}, work, 96, 1, step));
    }
    CHECK_FALSE(detail::wheel_resized_rect(client, {}, {}, work, 96, 1, 5));
    CHECK_FALSE(detail::wheel_resized_rect(client, {16, 9}, {}, work, 0, 1, 5));
    CHECK_FALSE(detail::wheel_resized_rect(client, {16, 9}, {-1, 0, 0, 0}, work, 96, 1, 5));
    CHECK_FALSE(detail::wheel_resized_rect(client, {16, 9}, {2000, 0, 2000, 0}, work, 96, 1, 5));
    CHECK_FALSE(detail::wheel_resized_rect({}, {16, 9}, {}, work, 96, 1, 5));
    CHECK_FALSE(detail::wheel_resized_rect({10, 10, 0, 0}, {16, 9}, {}, work, 96, 1, 5));
    CHECK_FALSE(detail::wheel_resized_rect(client, {16, 9}, {}, {}, 96, 1, 5));
    constexpr detail::NativeRect overflow{(std::numeric_limits<int>::min)(), 0,
                                         (std::numeric_limits<int>::max)(), 100};
    CHECK_FALSE(detail::wheel_resized_rect(overflow, {16, 9}, {}, work, 96, 1, 5));
    CHECK_FALSE(detail::wheel_resized_rect(client, {16, 9}, {}, overflow, 96, 1, 5));
    constexpr auto far_edge = (std::numeric_limits<std::int32_t>::max)();
    const auto far_monitor = detail::wheel_resized_rect(
      {far_edge - 800, far_edge - 800, far_edge - 160, far_edge - 440}, {16, 9}, {},
      {far_edge - 1920, far_edge - 1080, far_edge, far_edge}, 96, 1, 1);
    REQUIRE(far_monitor);
    CHECK(far_monitor->right - far_monitor->left == 646);
    CHECK(far_monitor->bottom - far_monitor->top == 364);
  }

  SECTION("native hidden wheel changes editable geometry")
  {
    const TestGlfwLifetime glfw;
    REQUIRE(glfw.initialized());
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    const auto focus = GetFocus();
    const auto capture = GetCapture();
    for (const auto border : {BorderPreference::Borderless, BorderPreference::Bordered}) {
      CAPTURE(border);
      auto window = overlay::OverlayWindow::create({640, 360}, border);
      REQUIRE(window);
      const auto hwnd = find_test_overlay_window();
      REQUIRE(hwnd != nullptr);
      REQUIRE_FALSE(IsWindowVisible(hwnd));
      auto detached = overlay::Win32OverlayAdapter::create_detached(
        **window, OverlayMode::Editable, border, {16, 9});
      REQUIRE(detached);
      const auto retire = [&](overlay::Win32OverlayAdapter *borrowed) noexcept {
        std::unique_ptr<overlay::Win32OverlayAdapter> owner{borrowed};
        if (owner->window_destruction_required() || !owner->detach()) {
          (*window)->destroy();
          owner->complete_after_overlay_destroyed();
        }
      };
      const std::unique_ptr<overlay::Win32OverlayAdapter, decltype(retire)>
        adapter{detached->release(), retire};
      REQUIRE(adapter->attach());
      REQUIRE(adapter->set_presentation(OverlayMode::Editable, 63));
      const auto snapshot = [&] {
        const auto geometry = adapter->snapshot_client_geometry();
        REQUIRE(geometry);
        return geometry->client_screen_rect;
      };
      // The pure-policy SECTION owns fixed numeric expectations. Native wiring
      // uses the actual monitor and frame metrics, including small work areas.
      const auto expected_client = [&](const int notches, const int percent,
                                        const Extent aspect = Extent{16, 9}) {
        MONITORINFO monitor{.cbSize = sizeof(MONITORINFO)};
        REQUIRE(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor));
        const auto dpi = GetDpiForWindow(hwnd);
        RECT adjusted{0, 0, 1, 1};
        REQUIRE(AdjustWindowRectExForDpi(&adjusted,
          static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
          static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi));
        const detail::FrameInsets insets{-adjusted.left, -adjusted.top,
                                          adjusted.right - 1, adjusted.bottom - 1};
        const auto &work = monitor.rcWork;
        const auto outer = detail::wheel_resized_rect(snapshot(), aspect, insets,
          {work.left, work.top, work.right, work.bottom}, dpi, notches, percent);
        REQUIRE(outer);
        return detail::outer_to_content_rect(*outer, insets);
      };
      const auto wheel = [&](const int delta, const bool inside = true) {
        const auto client_rect = snapshot();
        const auto x = inside ? client_rect.left + (client_rect.right - client_rect.left) / 2
                              : client_rect.left - 1;
        const auto y = client_rect.top + (client_rect.bottom - client_rect.top) / 2;
        SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)),
          MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y)));
      };
      const auto initial = snapshot();
      wheel(30);
      wheel(30);
      CHECK(snapshot() == initial);
      const auto expected_enlarged = expected_client(1, 5);
      wheel(60);
      const auto enlarged = snapshot();
      CHECK(enlarged == expected_enlarged);
      const auto expected_reduced = expected_client(-1, 5);
      wheel(-120);
      CHECK(snapshot() == expected_reduced);
      const auto restored = snapshot();
      wheel(120, false);
      CHECK(snapshot() == restored);

      wheel(60);
      REQUIRE(adapter->set_mode(OverlayMode::PassThrough));
      const auto pass_through = snapshot();
      wheel(120);
      CHECK(snapshot() == pass_through);
      REQUIRE(adapter->set_mode(OverlayMode::Editable));
      wheel(60);
      CHECK(snapshot() == pass_through);
      const auto expected_after_mode = expected_client(1, 5);
      wheel(60);
      CHECK(snapshot() == expected_after_mode);

      wheel(60);
      adapter->set_wheel_zoom_percent(25);
      const auto before_step = snapshot();
      wheel(60);
      CHECK(snapshot() == before_step);
      const auto expected_two_steps = expected_client(2, 25);
      wheel(180);
      CHECK(snapshot() == expected_two_steps);

      wheel(60);
      adapter->set_region_selection(true); // idle selection never captures or focuses
      const auto selecting = snapshot();
      wheel(120);
      CHECK(snapshot() == selecting);
      adapter->set_region_selection(false);
      wheel(60);
      CHECK(snapshot() == selecting);
      adapter->set_wheel_zoom_percent(5);
      adapter->update_source_extent({4, 3}); // runtime supplies the effective crop aspect
      wheel(-120);
      const auto cropped = snapshot();
      CHECK(std::abs((cropped.right - cropped.left) * 3 -
                     (cropped.bottom - cropped.top) * 4) <= 2);
      wheel(60);
      wheel(120, false); // outside input must not consume the accepted partial delta
      CHECK(snapshot() == cropped);
      adapter->set_wheel_zoom_percent(0); // an invalid direct setter is ignored
      const auto expected_after_outside = expected_client(1, 5, {4, 3});
      wheel(60);
      CHECK(snapshot() == expected_after_outside);
      CHECK(adapter->mode() == OverlayMode::Editable);
      CHECK(adapter->opacity_percent() == 63);
      CHECK(adapter->border_preference() == border);
      CHECK((*window)->border_matches(border));
      CHECK_FALSE(IsWindowVisible(hwnd));
      CHECK(GetForegroundWindow() != hwnd);
      CHECK(GetFocus() == focus);
      CHECK(GetCapture() == capture);
    }
  }

  SECTION("DPI band and aspect sizing preserve opposite anchors")
  {
    CHECK(detail::scaled_resize_band(8, 96) == 8);
    CHECK(detail::scaled_resize_band(8, 144) == 12);
    CHECK(detail::scaled_resize_band(8, 192) == 16);

    const Extent source{.width = 16, .height = 9};
    const detail::NativeRect horizontal{
      .left = 100, .top = 200, .right = 580, .bottom = 600};
    const auto from_right = detail::aspect_constrained_rect(
      horizontal, detail::SizingEdge::Right, source);
    CHECK(from_right.left == horizontal.left);
    CHECK(from_right.top == horizontal.top);
    CHECK(from_right.right == horizontal.right);
    CHECK(from_right.bottom == 470);

    const detail::NativeRect vertical{
      .left = 100, .top = 200, .right = 800, .bottom = 650};
    const auto from_top = detail::aspect_constrained_rect(
      vertical, detail::SizingEdge::Top, source);
    CHECK(from_top.left == vertical.left);
    CHECK(from_top.right == 900);
    CHECK(from_top.bottom == vertical.bottom);
    CHECK(from_top.top == vertical.top);

    const detail::NativeRect bordered_outer{
      .left = 100, .top = 200, .right = 596, .bottom = 600};
    const auto bordered_from_right = detail::aspect_constrained_rect(
      bordered_outer, detail::SizingEdge::Right, source,
      {.left = 8, .top = 31, .right = 8, .bottom = 8});
    CHECK(bordered_from_right.left == bordered_outer.left);
    CHECK(bordered_from_right.top == bordered_outer.top);
    CHECK(bordered_from_right.right == bordered_outer.right);
    CHECK(bordered_from_right.bottom == 509);
  }

  SECTION("border geometry converts content without owning decoration bits")
  {
    const detail::NativeRect content{
      .left = 120, .top = 160, .right = 920, .bottom = 610};
    CHECK(detail::content_to_outer_rect(
            content, {.left = 0, .top = 0, .right = 0, .bottom = 0}) ==
          content);
    CHECK(detail::content_to_outer_rect(
            content, {.left = 8, .top = 31, .right = 8, .bottom = 8}) ==
          detail::NativeRect{
            .left = 112, .top = 129, .right = 928, .bottom = 618});

    const detail::NativeRect work_area{
      .left = 0, .top = 0, .right = 1920, .bottom = 1040};
    const detail::NativeRect near_origin{
      .left = -8, .top = -31, .right = 808, .bottom = 458};
    const auto clamped =
      detail::clamp_outer_rect_to_work_area(near_origin, work_area);
    CHECK(clamped ==
          detail::NativeRect{.left = 0, .top = 0, .right = 816, .bottom = 489});
    CHECK(
      detail::outer_to_content_rect(
        clamped, {.left = 8, .top = 31, .right = 8, .bottom = 8}) ==
      detail::NativeRect{.left = 8, .top = 31, .right = 808, .bottom = 481});
    CHECK(detail::clamp_outer_rect_to_work_area(
            {.left = (std::numeric_limits<std::int32_t>::min)(),
             .top = (std::numeric_limits<std::int32_t>::min)(),
             .right = (std::numeric_limits<std::int32_t>::max)(),
             .bottom = (std::numeric_limits<std::int32_t>::max)()},
            work_area) == work_area);
  }

  SECTION("settings transaction restores every failed stage")
  {
    constexpr std::array failure_stages{
      SettingsCall::Mode,
      SettingsCall::Border,
      SettingsCall::Geometry,
      SettingsCall::Extensions,
      SettingsCall::VerifyApplied,
    };
    for (const auto failure_stage : failure_stages) {
      CAPTURE(failure_stage);
      RecordingSettingsOperations operations{.fail = failure_stage};
      const auto result = detail::apply_settings_transaction(operations);
      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().code == AppErrorCode::PlatformOperationFailure);
      CHECK(operations.calls[operations.calls.size() - 2] ==
            SettingsCall::Rollback);
      CHECK(operations.calls.back() == SettingsCall::VerifyRollback);
      CHECK(std::find(operations.calls.begin(), operations.calls.end(),
                      SettingsCall::CommitScheduler) == operations.calls.end());
    }

    constexpr std::array unverified_rollback_stages{
      SettingsCall::Geometry,
      SettingsCall::Extensions,
    };
    for (const auto failure_stage : unverified_rollback_stages) {
      CAPTURE(failure_stage);
      RecordingSettingsOperations lost{.fail = failure_stage,
                                       .rollback_verifies = false};
      const auto lost_result = detail::apply_settings_transaction(lost);
      REQUIRE_FALSE(lost_result.has_value());
      CHECK(lost_result.error().code == AppErrorCode::RuntimeStateLost);
      CHECK(std::find(lost.calls.begin(), lost.calls.end(),
                      SettingsCall::CommitScheduler) == lost.calls.end());
    }

    RecordingSettingsOperations restored_despite_reported_failure{
      .fail = SettingsCall::Geometry, .rollback_succeeds = false};
    const auto verified_result =
      detail::apply_settings_transaction(restored_despite_reported_failure);
    REQUIRE_FALSE(verified_result.has_value());
    CHECK(verified_result.error().code ==
          AppErrorCode::PlatformOperationFailure);
    CHECK(restored_despite_reported_failure.calls.back() ==
          SettingsCall::VerifyRollback);

    RecordingSettingsOperations exceptional{.throw_from = SettingsCall::Border};
    const auto exceptional_result =
      detail::apply_settings_transaction(exceptional);
    REQUIRE_FALSE(exceptional_result.has_value());
    CHECK(exceptional_result.error().code == AppErrorCode::RuntimeStateLost);
    CHECK(exceptional.calls[exceptional.calls.size() - 2] ==
          SettingsCall::Rollback);
    CHECK(exceptional.calls.back() == SettingsCall::VerifyRollback);

    RecordingSettingsOperations exceptional_extensions{
      .throw_from = SettingsCall::Extensions};
    const auto exceptional_extensions_result =
      detail::apply_settings_transaction(exceptional_extensions);
    REQUIRE_FALSE(exceptional_extensions_result.has_value());
    CHECK(exceptional_extensions_result.error().code ==
          AppErrorCode::RuntimeStateLost);
    CHECK(exceptional_extensions.calls[exceptional_extensions.calls.size() - 2] ==
          SettingsCall::Rollback);
    CHECK(exceptional_extensions.calls.back() == SettingsCall::VerifyRollback);

    RecordingSettingsOperations source_lost_while_applying{
      .fail = SettingsCall::VerifyApplied,
      .helper_active = true,
      .source_closes_before_extensions = true,
    };
    const auto source_lost_result =
      detail::apply_settings_transaction(source_lost_while_applying);
    REQUIRE_FALSE(source_lost_result.has_value());
    CHECK(source_lost_result.error().code == AppErrorCode::RuntimeStateLost);
    CHECK(source_lost_while_applying.helper_hide_count == 1);
    CHECK(source_lost_while_applying.helper_show_count == 0);
    CHECK_FALSE(source_lost_while_applying.helper_active);
    CHECK(source_lost_while_applying.rollback_calls ==
          std::vector{SettingsRollbackCall::Helper, SettingsRollbackCall::Mode,
                      SettingsRollbackCall::Border,
                      SettingsRollbackCall::Geometry});
    CHECK(std::find(source_lost_while_applying.calls.begin(),
                    source_lost_while_applying.calls.end(),
                    SettingsCall::CommitScheduler) ==
          source_lost_while_applying.calls.end());

    RecordingSettingsOperations success;
    REQUIRE(detail::apply_settings_transaction(success).has_value());
    CHECK(success.calls ==
          std::vector{SettingsCall::Snapshot, SettingsCall::Mode,
                      SettingsCall::Border, SettingsCall::Geometry,
                      SettingsCall::Extensions, SettingsCall::VerifyApplied,
                      SettingsCall::CommitScheduler});
  }

  SECTION("WndProc contains exceptions and clears only after NC destroy")
  {
    RecordingCallbackOperations exceptional{.throw_while_preparing = true};
    static_assert(noexcept(detail::dispatch_overlay_callback(
      exceptional, WM_SIZE, WPARAM{}, LPARAM{})));
    CHECK(detail::dispatch_overlay_callback(exceptional, WM_SIZE, 0, 0) == 73);
    CHECK(exceptional.committed_mutations == 0);
    CHECK(exceptional.forwards == 1);
    CHECK(exceptional.binding_live);

    RecordingCallbackOperations handled;
    CHECK(detail::dispatch_overlay_callback(handled, WM_NCHITTEST, 0, 0) == 91);
    CHECK(handled.committed_mutations == 1);
    CHECK(handled.forwards == 0);

    RecordingCallbackOperations dpi_success{
      .prepared = {.result = 0,
                   .forward_after_commit = true,
                   .dpi = std::uint32_t{192}}};
    CHECK(detail::dispatch_overlay_callback(dpi_success, WM_DPICHANGED, 0, 0) ==
          73);
    CHECK(dpi_success.observed_dpi == 192);
    CHECK(dpi_success.order == std::vector{1, 2, 3});

    RecordingCallbackOperations dpi_rect_failure{
      .prepared = {.result = 0,
                   .forward_after_commit = true,
                   .dpi = std::uint32_t{0}},
      .fail_rect_application = true,
      .observed_dpi = 144};
    CHECK(detail::dispatch_overlay_callback(dpi_rect_failure, WM_DPICHANGED, 0,
                                            0) == 73);
    CHECK(dpi_rect_failure.observed_dpi == 144);
    CHECK(dpi_rect_failure.committed_mutations == 0);
    CHECK(dpi_rect_failure.order == std::vector{1, 2, 3});

    std::uint32_t transactional_dpi{144};
    bool rectangle_attempted{};
    CHECK_FALSE(detail::apply_dpi_change_transaction(
      transactional_dpi, 192,
      {.left = 0, .top = 0, .right = 0, .bottom = 480},
      [&](int, int, int, int) noexcept {
        rectangle_attempted = true;
        return true;
      }));
    CHECK_FALSE(rectangle_attempted);
    CHECK(transactional_dpi == 144);
    CHECK_FALSE(detail::apply_dpi_change_transaction(
      transactional_dpi, 192,
      {.left = 0, .top = 0, .right = 640, .bottom = 480},
      [&](int, int, int, int) noexcept {
        rectangle_attempted = true;
        return false;
      }));
    CHECK(rectangle_attempted);
    CHECK(transactional_dpi == 144);
    CHECK(detail::apply_dpi_change_transaction(
      transactional_dpi, 0,
      {.left = 0, .top = 0, .right = 640, .bottom = 480},
      [](int, int, int, int) noexcept { return true; }));
    CHECK(transactional_dpi == 96);

    RecordingCallbackOperations destroyed;
    CHECK(detail::dispatch_overlay_callback(destroyed, WM_NCDESTROY, 0, 0) ==
          73);
    CHECK(destroyed.order == std::vector{3, 4});
    CHECK_FALSE(destroyed.binding_live);
  }

  SECTION("armed factory guard preserves adapter through failed detach")
  {
    struct FailedInstallRow final {
      detail::FailedWndProcInstallObservation proc;
      detail::OverlayPropertyRemoval property;
      bool native_binding_safe;
      bool destroy_with_adapter_alive;
    };
    constexpr std::array failed_install_rows{
      FailedInstallRow{detail::FailedWndProcInstallObservation::Original,
                       detail::OverlayPropertyRemoval::RemovedExpected, true,
                       false},
      FailedInstallRow{detail::FailedWndProcInstallObservation::Original,
                       detail::OverlayPropertyRemoval::StillPresent, false,
                       true},
      FailedInstallRow{detail::FailedWndProcInstallObservation::Custom,
                       detail::OverlayPropertyRemoval::AbsentUnexpected,
                       false, true},
      FailedInstallRow{detail::FailedWndProcInstallObservation::Custom,
                       detail::OverlayPropertyRemoval::StillPresent, false,
                       true},
      FailedInstallRow{detail::FailedWndProcInstallObservation::Unavailable,
                       detail::OverlayPropertyRemoval::AbsentUnexpected,
                       false, true},
      FailedInstallRow{detail::FailedWndProcInstallObservation::Unavailable,
                       detail::OverlayPropertyRemoval::StillPresent, false,
                       true},
    };
    for (const auto &row : failed_install_rows) {
      CAPTURE(row.proc, row.property);
      const auto decision =
        detail::failed_attachment_cleanup(row.proc, row.property);
      CHECK(decision.native_binding_safe == row.native_binding_safe);
      CHECK(decision.destroy_with_adapter_alive ==
            row.destroy_with_adapter_alive);
      RecordingAttachmentOperations failed_install{
        .callback_owned =
          row.proc != detail::FailedWndProcInstallObservation::Original,
        .property_live =
          row.property == detail::OverlayPropertyRemoval::StillPresent,
        .window_destruction_required = decision.destroy_with_adapter_alive};
      {
        detail::FactoryOverlayAttachmentGuard guard{failed_install};
      }
      CHECK(failed_install.order == (row.destroy_with_adapter_alive
                                       ? std::vector{2, 3}
                                       : std::vector<int>{}));
      CHECK(failed_install.adapter_was_alive_during_destroy ==
            row.destroy_with_adapter_alive);
      CHECK_FALSE(failed_install.property_live);
      CHECK_FALSE(failed_install.window_destruction_required);
      const auto forwarded =
        row.destroy_with_adapter_alive &&
        row.proc != detail::FailedWndProcInstallObservation::Original &&
        row.property == detail::OverlayPropertyRemoval::StillPresent;
      CHECK(failed_install.callback.order ==
            (forwarded ? std::vector{3, 4} : std::vector<int>{}));
    }

    RecordingAttachmentOperations ordinary;
    try {
      detail::FactoryOverlayAttachmentGuard guard{ordinary};
      guard.arm();
      throw std::runtime_error{"later construction failed"};
    } catch (const std::runtime_error &) {
    }
    CHECK(ordinary.order == std::vector{1});
    CHECK_FALSE(ordinary.callback_owned);
    CHECK_FALSE(ordinary.property_live);

    RecordingAttachmentOperations restoration_failed{.restore_wndproc_succeeds =
                                                       false};
    try {
      detail::FactoryOverlayAttachmentGuard guard{restoration_failed};
      guard.arm();
      throw std::runtime_error{"later construction failed"};
    } catch (const std::runtime_error &) {
    }
    CHECK(restoration_failed.order == std::vector{1, 2, 3});
    CHECK(restoration_failed.adapter_was_alive_during_destroy);
    CHECK_FALSE(restoration_failed.callback_owned);
    CHECK_FALSE(restoration_failed.property_live);
    CHECK(restoration_failed.adapter_alive);
    CHECK(restoration_failed.callback.order == std::vector{3, 4});

    RecordingAttachmentOperations partial_detach{
      .property_removal = detail::OverlayPropertyRemoval::StillPresent};
    try {
      detail::FactoryOverlayAttachmentGuard guard{partial_detach};
      guard.arm();
      throw std::runtime_error{"later construction failed"};
    } catch (const std::runtime_error &) {
    }
    CHECK(partial_detach.order == std::vector{1, 2, 3});
    CHECK(partial_detach.detach_error == AppErrorCode::RuntimeStateLost);
    CHECK(partial_detach.adapter_was_alive_during_destroy);
    CHECK_FALSE(partial_detach.callback_owned);
    CHECK_FALSE(partial_detach.property_live);
    CHECK_FALSE(partial_detach.window_destruction_required);
    CHECK(partial_detach.callback.order.empty());
  }
}
