#include "platform/windows/detail/frame_intake_gate.hpp"
#include "platform/windows/detail/item_observation_state.hpp"
#include "platform/windows/detail/ui_language_query.hpp"
#include "platform/windows/detail/wgc_capture_state.hpp"
#include "platform/windows/wgc_capture_job.hpp"
#include "platform/windows/windows_capture_source.hpp"
#include "platform/windows/windows_captured_frame.hpp"
#include "platform/windows/windows_global_hotkey.hpp"
#include "platform/windows/windows_source_catalog.hpp"
#include "platform/windows/windows_source_registry.hpp"

#include "avb/app/commands.hpp"
#include "avb/core/latest_value_mailbox.hpp"
#include "avb/core/types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <dxgi1_6.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using WindowsSourceLease = avb::platform::windows::WindowsSourceLease;
using WindowsSourceRegistry =
  avb::platform::windows::WindowsSourceRegistry;
using WindowsCapturedFrame = avb::platform::windows::WindowsCapturedFrame;
using WindowsCaptureSource = avb::platform::windows::WindowsCaptureSource;

static_assert(std::is_final_v<WindowsCapturedFrame>);
static_assert(std::move_constructible<WindowsCapturedFrame>);
static_assert(std::is_nothrow_move_constructible_v<WindowsCapturedFrame>);
static_assert(std::is_nothrow_move_assignable_v<WindowsCapturedFrame>);
static_assert(!std::copy_constructible<WindowsCapturedFrame>);
static_assert(!std::is_copy_assignable_v<WindowsCapturedFrame>);
static_assert(std::is_nothrow_destructible_v<WindowsCapturedFrame>);
static_assert(std::same_as<
              decltype(std::declval<const WindowsCapturedFrame &>().sequence()),
              std::uint64_t>);
static_assert(std::same_as<decltype(std::declval<const WindowsCapturedFrame &>()
                                      .resource_generation()),
                           std::uint64_t>);
static_assert(std::same_as<decltype(std::declval<const WindowsCapturedFrame &>()
                                      .allocation_extent()),
                           avb::core::Extent>);
static_assert(std::same_as<decltype(std::declval<const WindowsCapturedFrame &>()
                                      .content_size()),
                           avb::core::Extent>);
static_assert(
  std::same_as<decltype(std::declval<const WindowsCapturedFrame &>()
                          .expected_pool_format()),
               winrt::Windows::Graphics::DirectX::DirectXPixelFormat>);
static_assert(std::same_as<decltype(std::declval<const WindowsCapturedFrame &>()
                                      .capture_timestamp()),
                           std::chrono::steady_clock::time_point>);
static_assert(std::same_as<
              decltype(std::declval<const WindowsCapturedFrame &>().texture()),
              Microsoft::WRL::ComPtr<ID3D11Texture2D>>);

static_assert(std::move_constructible<WindowsSourceLease>);
static_assert(std::is_nothrow_move_constructible_v<WindowsSourceLease>);
static_assert(!std::copy_constructible<WindowsSourceLease>);
static_assert(
  std::same_as<decltype(std::declval<const WindowsSourceRegistry &>().resolve(
                 std::declval<const avb::core::SourceId &>())),
               std::expected<WindowsSourceLease, avb::core::AppError>>);
namespace {

  namespace hotkey_detail = avb::platform::windows::detail;

  enum class HotkeyCallKind { Register,
                              Unregister };

  struct HotkeyCall {
    HotkeyCallKind kind;
    int id;
    std::optional<hotkey_detail::NativeHotkey> hotkey;
  };

  struct ScriptedHotkeyCalls {
    std::deque<hotkey_detail::NativeCallResult> register_results;
    std::deque<hotkey_detail::NativeCallResult> unregister_results;
    std::vector<HotkeyCall> calls;

    [[nodiscard]] hotkey_detail::RegisterHotkeyFn register_fn()
    {
      return [this](int id, hotkey_detail::NativeHotkey hotkey) {
        calls.push_back(HotkeyCall{HotkeyCallKind::Register, id, hotkey});
        return next(register_results);
      };
    }

    [[nodiscard]] hotkey_detail::UnregisterHotkeyFn unregister_fn()
    {
      return [this](int id) {
        calls.push_back(HotkeyCall{HotkeyCallKind::Unregister, id, std::nullopt});
        return next(unregister_results);
      };
    }

  private:
    [[nodiscard]] static hotkey_detail::NativeCallResult
    next(std::deque<hotkey_detail::NativeCallResult> &results)
    {
      if (results.empty()) {
        return {.succeeded = true};
      }
      const auto result = results.front();
      results.pop_front();
      return result;
    }
  };

  class ControlledCaptureFrame
      : public winrt::implements<
          ControlledCaptureFrame,
          winrt::Windows::Graphics::Capture::IDirect3D11CaptureFrame,
          winrt::Windows::Foundation::IClosable> {
  public:
    explicit ControlledCaptureFrame(std::shared_ptr<std::uint32_t> close_count)
        : close_count_(std::move(close_count)) {}

    [[nodiscard]] winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface
    Surface() const
    {
      return nullptr;
    }

    [[nodiscard]] winrt::Windows::Foundation::TimeSpan
    SystemRelativeTime() const noexcept
    {
      return {};
    }

    [[nodiscard]] winrt::Windows::Graphics::SizeInt32
    ContentSize() const noexcept
    {
      return {.Width = 1280, .Height = 720};
    }

    void Close() noexcept { ++*close_count_; }

  private:
    std::shared_ptr<std::uint32_t> close_count_;
  };

  [[nodiscard]] winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame
  make_controlled_capture_frame(
    const std::shared_ptr<std::uint32_t> &close_count)
  {
    return winrt::make<ControlledCaptureFrame>(close_count)
      .as<winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame>();
  }

  namespace capture_detail = avb::platform::windows::detail;
  using PixelFormat = winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

  class ControlledCaptureItem
      : public winrt::implements<
          ControlledCaptureItem,
          winrt::Windows::Graphics::Capture::IGraphicsCaptureItem> {
  public:
    [[nodiscard]] winrt::hstring DisplayName() const { return L"controlled"; }

    [[nodiscard]] winrt::Windows::Graphics::SizeInt32 Size() const noexcept
    {
      return {.Width = 640, .Height = 480};
    }

    [[nodiscard]] winrt::event_token
    Closed(const winrt::Windows::Foundation::TypedEventHandler<
           winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
           winrt::Windows::Foundation::IInspectable> &handler)
    {
      ++registration_count;
      handler_ = handler;
      return {.value = 1};
    }

    void Closed(const winrt::event_token &) noexcept
    {
      ++revocation_count;
      late_handler_ = handler_;
      handler_ = nullptr;
    }

    void fire()
    {
      if (handler_) {
        handler_(nullptr, nullptr);
      }
    }

    void fire_late()
    {
      if (late_handler_) {
        late_handler_(nullptr, nullptr);
      }
    }

    std::uint32_t registration_count{};
    std::uint32_t revocation_count{};

  private:
    winrt::Windows::Foundation::TypedEventHandler<
      winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
      winrt::Windows::Foundation::IInspectable>
      handler_;
    winrt::Windows::Foundation::TypedEventHandler<
      winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
      winrt::Windows::Foundation::IInspectable>
      late_handler_;
  };

  struct FakeCheckoutStats final {
    std::uint32_t moves{};
    std::uint32_t destructions{};
    std::function<void()> on_destroy;
  };

  class FakeCheckout final {
  public:
    FakeCheckout(std::uint64_t sequence, std::uint64_t generation, avb::core::Extent allocation, avb::core::Extent content, PixelFormat format, std::shared_ptr<FakeCheckoutStats> stats, std::weak_ptr<int> pool_owner = {})
        : sequence_{sequence}, generation_{generation}, allocation_{allocation}, content_{content}, format_{format}, stats_{std::move(stats)}, pool_owner_{std::move(pool_owner)}, owns_{true} {}

    ~FakeCheckout() noexcept { release(); }

    FakeCheckout(const FakeCheckout &) = delete;
    FakeCheckout &operator=(const FakeCheckout &) = delete;

    FakeCheckout(FakeCheckout &&other) noexcept
        : sequence_{other.sequence_}, generation_{other.generation_}, allocation_{other.allocation_}, content_{other.content_}, format_{other.format_}, stats_{std::move(other.stats_)}, pool_owner_{std::move(other.pool_owner_)}, owns_{std::exchange(other.owns_, false)}
    {
      if (stats_ && owns_) {
        ++stats_->moves;
      }
    }

    FakeCheckout &operator=(FakeCheckout &&other) noexcept
    {
      if (this != &other) {
        release();
        sequence_ = other.sequence_;
        generation_ = other.generation_;
        allocation_ = other.allocation_;
        content_ = other.content_;
        format_ = other.format_;
        stats_ = std::move(other.stats_);
        pool_owner_ = std::move(other.pool_owner_);
        owns_ = std::exchange(other.owns_, false);
        if (stats_ && owns_) {
          ++stats_->moves;
        }
      }
      return *this;
    }

    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    [[nodiscard]] std::uint64_t resource_generation() const noexcept
    {
      return generation_;
    }
    [[nodiscard]] avb::core::Extent allocation_extent() const noexcept
    {
      return allocation_;
    }
    [[nodiscard]] avb::core::Extent content_size() const noexcept
    {
      return content_;
    }
    [[nodiscard]] PixelFormat expected_pool_format() const noexcept
    {
      return format_;
    }
    void set_content_size(const avb::core::Extent content) noexcept
    {
      content_ = content;
    }
    [[nodiscard]] bool pool_owner_alive() const noexcept
    {
      return !pool_owner_.expired();
    }

  private:
    void release() noexcept
    {
      if (!std::exchange(owns_, false) || !stats_) {
        return;
      }
      ++stats_->destructions;
      if (stats_->on_destroy) {
        try {
          stats_->on_destroy();
        } catch (...) {
        }
      }
    }

    std::uint64_t sequence_{};
    std::uint64_t generation_{};
    avb::core::Extent allocation_{};
    avb::core::Extent content_{};
    PixelFormat format_{};
    std::shared_ptr<FakeCheckoutStats> stats_;
    std::weak_ptr<int> pool_owner_;
    bool owns_{};
  };

  using FakeFrameMailbox = avb::core::LatestValueMailbox<FakeCheckout>;

  [[nodiscard]] FakeCheckout make_fake_checkout(
    std::uint64_t sequence, const std::shared_ptr<FakeCheckoutStats> &stats, avb::core::Extent allocation = {.width = 640, .height = 480}, avb::core::Extent content = {.width = 640, .height = 480}, PixelFormat format = PixelFormat::B8G8R8A8UIntNormalized, std::uint64_t generation = 7, std::weak_ptr<int> pool_owner = {})
  {
    return FakeCheckout{sequence, generation, allocation, content,
                        format, stats, std::move(pool_owner)};
  }

  constexpr wchar_t destruction_invariant_child_environment[] =
    L"AVB0795_WGC_DESTRUCTION_INVARIANT_CHILD";
  constexpr DWORD destruction_invariant_exit_code = 86;

  [[nodiscard]] std::optional<DWORD> run_destruction_invariant_child()
  {
    std::array<wchar_t, 32'768> executable{};
    const auto path_size = GetModuleFileNameW(
      nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (path_size == 0 || path_size == executable.size()) {
      return std::nullopt;
    }

    std::wstring command_line = L"\"";
    command_line.append(executable.data(), path_size);
    command_line += L"\" \"avb0795 V1 case 12*\" --reporter compact";

    if (!SetEnvironmentVariableW(destruction_invariant_child_environment, L"1")) {
      return std::nullopt;
    }
    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process{};
    const auto created = CreateProcessW(executable.data(), command_line.data(),
                                        nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    static_cast<void>(SetEnvironmentVariableW(
      destruction_invariant_child_environment, nullptr));
    if (!created) {
      return std::nullopt;
    }

    const auto wait = WaitForSingleObject(process.hProcess, 5'000);
    if (wait != WAIT_OBJECT_0) {
      static_cast<void>(TerminateProcess(process.hProcess, 87));
      static_cast<void>(WaitForSingleObject(process.hProcess, 5'000));
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return std::nullopt;
    }

    DWORD exit_code{};
    const auto queried = GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!queried) {
      return std::nullopt;
    }
    return exit_code;
  }

  [[nodiscard]] constexpr avb::core::HotkeyChord
  ctrl_chord(avb::core::HotkeyKey key) noexcept
  {
    return {
      .modifiers = {.control = true},
      .key = key,
    };
  }

  [[nodiscard]] hotkey_detail::HotkeyRegistrationState
  confirmed_state(const avb::core::HotkeyChord &chord)
  {
    return {
      .primary = hotkey_detail::FixedHotkeyState::Registered,
      .probe = hotkey_detail::FixedHotkeyState::Free,
      .confirmed_chord = chord,
    };
  }

  [[nodiscard]] LPARAM
  hotkey_message_parameter(const avb::core::HotkeyChord &chord)
  {
    const auto native = hotkey_detail::map_hotkey_to_native(chord);
    REQUIRE(native.has_value());
    return MAKELPARAM(static_cast<WORD>(native->modifiers),
                      static_cast<WORD>(native->virtual_key));
  }

}    // namespace

TEST_CASE("avb0795 V1 case 09: Windows source discovery policies",
          "[avb0795-v1]")
{
  using avb::core::AppErrorCode;
  using avb::core::ColorSupport;
  using avb::platform::windows::classify_color_support;
  using avb::platform::windows::is_window_source_eligible;
  using avb::platform::windows::WindowEligibilityFacts;
  using avb::platform::windows::detail::WindowsSourceIdGeneration;

  SECTION("preferred UI languages use one bounded table-driven native query")
  {
    using avb::i18n::Language;
    using avb::platform::windows::detail::read_ui_language;

    enum class Scenario {
      Success,
      ProbeFailure,
      FillFailure,
      ExcessRequiredSize,
      MissingDoubleTerminator,
      UnsupportedOnly,
    };
    struct Row {
      std::string_view name;
      Scenario scenario;
      Language expected;
    };

    constexpr std::array rows{
      Row{"first supported language", Scenario::Success, Language::Japanese},
      Row{"probe failure", Scenario::ProbeFailure, Language::English},
      Row{"fill failure", Scenario::FillFailure, Language::English},
      Row{"excess required size", Scenario::ExcessRequiredSize,
          Language::English},
      Row{"missing double terminator", Scenario::MissingDoubleTerminator,
          Language::English},
      Row{"unsupported languages", Scenario::UnsupportedOnly,
          Language::English},
    };
    constexpr wchar_t preferred[] = L"fr-FR\0ja-JP\0en-US\0";
    constexpr wchar_t missing_double_terminator[] =
      L"fr-FR\0ja-JP\0en-US";
    constexpr wchar_t unsupported[] = L"fr-FR\0de-DE\0";
    Scenario scenario{Scenario::Success};
    const auto query = [&](DWORD flags, PULONG count, PZZWSTR buffer,
                           PULONG size) -> BOOL {
      CHECK(flags == MUI_LANGUAGE_NAME);
      REQUIRE(count != nullptr);
      REQUIRE(size != nullptr);

      if (buffer == nullptr) {
        CHECK(*size == 0);
        if (scenario == Scenario::ProbeFailure) {
          return FALSE;
        }
        if (scenario == Scenario::ExcessRequiredSize) {
          *size = 32'769;
          return TRUE;
        }
        if (scenario == Scenario::MissingDoubleTerminator) {
          *count = 3;
          *size = static_cast<ULONG>(std::size(missing_double_terminator));
          return TRUE;
        }
        if (scenario == Scenario::UnsupportedOnly) {
          *count = 2;
          *size = static_cast<ULONG>(std::size(unsupported));
          return TRUE;
        }
        *count = 3;
        *size = static_cast<ULONG>(std::size(preferred));
        return TRUE;
      }

      CHECK(*count ==
            (scenario == Scenario::UnsupportedOnly ? ULONG{2} : ULONG{3}));
      if (scenario == Scenario::FillFailure) {
        return FALSE;
      }
      if (scenario == Scenario::MissingDoubleTerminator) {
        REQUIRE(*size >= std::size(missing_double_terminator));
        std::ranges::copy(missing_double_terminator, buffer);
        *size = static_cast<ULONG>(std::size(missing_double_terminator));
        return TRUE;
      }
      if (scenario == Scenario::UnsupportedOnly) {
        REQUIRE(*size >= std::size(unsupported));
        std::ranges::copy(unsupported, buffer);
        *size = static_cast<ULONG>(std::size(unsupported));
        return TRUE;
      }
      REQUIRE(*size >= std::size(preferred));
      std::ranges::copy(preferred, buffer);
      *size = static_cast<ULONG>(std::size(preferred));
      return TRUE;
    };

    for (const auto &row : rows) {
      CAPTURE(row.name);
      scenario = row.scenario;
      CHECK(read_ui_language(query) == row.expected);
    }
  }

  SECTION("window eligibility is one table-driven policy")
  {
    struct Row {
      std::string_view name;
      WindowEligibilityFacts facts;
      bool expected;
    };

    constexpr WindowEligibilityFacts normal{
      .visible = true,
      .cloak_query_succeeded = true,
      .cloaked = false,
      .has_nonzero_area = true,
      .tool_window = false,
      .tool_window_intended_for_selection = false,
      .owned_by_current_process = false,
      .has_usable_title = true,
    };
    constexpr std::array rows{
      Row{"visible", normal, true},
      Row{"invisible",
          [](auto facts) {
            facts.visible = false;
            return facts;
          }(normal),
          false},
      Row{"cloaked",
          [](auto facts) {
            facts.cloaked = true;
            return facts;
          }(normal),
          false},
      Row{"cloak query failed",
          [](auto facts) {
            facts.cloak_query_succeeded = false;
            return facts;
          }(normal),
          false},
      Row{"zero-size",
          [](auto facts) {
            facts.has_nonzero_area = false;
            return facts;
          }(normal),
          false},
      Row{"tool window",
          [](auto facts) {
            facts.tool_window = true;
            return facts;
          }(normal),
          false},
      Row{"intended tool window",
          [](auto facts) {
            facts.tool_window = true;
            facts.tool_window_intended_for_selection = true;
            return facts;
          }(normal),
          true},
      Row{"current process",
          [](auto facts) {
            facts.owned_by_current_process = true;
            return facts;
          }(normal),
          false},
      Row{"no usable title",
          [](auto facts) {
            facts.has_usable_title = false;
            return facts;
          }(normal),
          false},
      Row{"normal", normal, true},
    };

    for (const auto &row : rows) {
      CAPTURE(row.name);
      CHECK(is_window_source_eligible(row.facts) == row.expected);
    }
  }

  SECTION("a source ID from the previous registry generation is stale")
  {
    WindowsSourceIdGeneration generation;
    const auto stale_id = generation.issue();
    REQUIRE(generation.validate(stale_id).has_value());

    generation.advance();

    const auto result = generation.validate(stale_id);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == AppErrorCode::SourceUnavailable);
  }

  SECTION("DXGI color observations map to portable source metadata")
  {
    struct Row {
      std::string_view name;
      std::optional<DXGI_COLOR_SPACE_TYPE> observed;
      ColorSupport expected;
    };

    constexpr std::array rows{
      Row{"HDR", DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
          ColorSupport::HdrDetectedUnsupported},
      Row{"SDR", DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, ColorSupport::Sdr},
      Row{"successful unrecognised", DXGI_COLOR_SPACE_CUSTOM,
          ColorSupport::UnknownAssumeSdr},
      Row{"failed or missing output", std::nullopt,
          ColorSupport::UnknownAssumeSdr},
    };

    for (const auto &row : rows) {
      CAPTURE(row.name);
      CHECK(classify_color_support(row.observed) == row.expected);
    }
  }
}

TEST_CASE("avb0795 V1 case 10: typed global hotkey transactions and messages",
          "[avb0795-v1]")
{
  using avb::core::AppErrorCode;
  using avb::core::HotkeyChord;
  using avb::core::HotkeyKey;
  using avb::platform::windows::HotkeyChordSink;

  static_assert(
    std::derived_from<avb::platform::windows::WindowsGlobalHotkey,
                      avb::app::IGlobalHotkey>);
  static_assert(noexcept(
    hotkey_detail::windows_global_hotkey_window_proc(nullptr, 0, 0, 0)));

  SECTION("typed modifiers and supported key groups map to Win32 values")
  {
    struct Row {
      HotkeyKey key;
      std::uint32_t expected;
    };
    constexpr std::array rows{
      Row{HotkeyKey::A, 0x41U},
      Row{HotkeyKey::B, 0x42U},
      Row{HotkeyKey::C, 0x43U},
      Row{HotkeyKey::D, 0x44U},
      Row{HotkeyKey::E, 0x45U},
      Row{HotkeyKey::F, 0x46U},
      Row{HotkeyKey::G, 0x47U},
      Row{HotkeyKey::H, 0x48U},
      Row{HotkeyKey::I, 0x49U},
      Row{HotkeyKey::J, 0x4AU},
      Row{HotkeyKey::K, 0x4BU},
      Row{HotkeyKey::L, 0x4CU},
      Row{HotkeyKey::M, 0x4DU},
      Row{HotkeyKey::N, 0x4EU},
      Row{HotkeyKey::O, 0x4FU},
      Row{HotkeyKey::P, 0x50U},
      Row{HotkeyKey::Q, 0x51U},
      Row{HotkeyKey::R, 0x52U},
      Row{HotkeyKey::S, 0x53U},
      Row{HotkeyKey::T, 0x54U},
      Row{HotkeyKey::U, 0x55U},
      Row{HotkeyKey::V, 0x56U},
      Row{HotkeyKey::W, 0x57U},
      Row{HotkeyKey::X, 0x58U},
      Row{HotkeyKey::Y, 0x59U},
      Row{HotkeyKey::Z, 0x5AU},
      Row{HotkeyKey::Digit0, 0x30U},
      Row{HotkeyKey::Digit1, 0x31U},
      Row{HotkeyKey::Digit2, 0x32U},
      Row{HotkeyKey::Digit3, 0x33U},
      Row{HotkeyKey::Digit4, 0x34U},
      Row{HotkeyKey::Digit5, 0x35U},
      Row{HotkeyKey::Digit6, 0x36U},
      Row{HotkeyKey::Digit7, 0x37U},
      Row{HotkeyKey::Digit8, 0x38U},
      Row{HotkeyKey::Digit9, 0x39U},
      Row{HotkeyKey::F1, VK_F1},
      Row{HotkeyKey::F2, VK_F2},
      Row{HotkeyKey::F3, VK_F3},
      Row{HotkeyKey::F4, VK_F4},
      Row{HotkeyKey::F5, VK_F5},
      Row{HotkeyKey::F6, VK_F6},
      Row{HotkeyKey::F7, VK_F7},
      Row{HotkeyKey::F8, VK_F8},
      Row{HotkeyKey::F9, VK_F9},
      Row{HotkeyKey::F10, VK_F10},
      Row{HotkeyKey::F11, VK_F11},
      Row{HotkeyKey::F12, VK_F12},
    };

    for (const auto &row : rows) {
      const HotkeyChord chord{
        .modifiers = {.control = true,
                      .alt = true,
                      .shift = true,
                      .meta = true},
        .key = row.key,
      };
      const auto mapped = hotkey_detail::map_hotkey_to_native(chord);
      CAPTURE(static_cast<int>(row.key));
      REQUIRE(mapped.has_value());
      CHECK(mapped->modifiers == (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN));
      CHECK(mapped->virtual_key == row.expected);
    }

    const auto invalid = hotkey_detail::map_hotkey_to_native(
      ctrl_chord(static_cast<HotkeyKey>(999)));
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == AppErrorCode::InvalidValue);

    constexpr std::array retryable_errors{
      AppErrorCode::HotkeyConflict,
      AppErrorCode::HotkeyCleanupFailure,
      AppErrorCode::HotkeyRegistrationLost,
    };
    for (const auto error_code : retryable_errors) {
      CAPTURE(static_cast<int>(error_code));
      CHECK(avb::core::AppError{.code = error_code}.retryable());
    }
  }

  SECTION("transaction construction rejects invalid concrete dependencies")
  {
    ScriptedHotkeyCalls api;
    const auto dispatch_state =
      std::make_shared<hotkey_detail::HotkeyDispatchState>(
        hotkey_detail::HotkeyRegistrationState{});

    const auto construct_without_state = [&api] {
      hotkey_detail::HotkeyRegistrationTransaction transaction{
        std::shared_ptr<hotkey_detail::HotkeyDispatchState>{},
        api.register_fn(), api.unregister_fn()};
    };
    const auto construct_without_register = [&api, &dispatch_state] {
      hotkey_detail::HotkeyRegistrationTransaction transaction{
        dispatch_state, hotkey_detail::RegisterHotkeyFn{},
        api.unregister_fn()};
    };
    const auto construct_without_unregister = [&api, &dispatch_state] {
      hotkey_detail::HotkeyRegistrationTransaction transaction{
        dispatch_state, api.register_fn(),
        hotkey_detail::UnregisterHotkeyFn{}};
    };

    CHECK_THROWS_AS(construct_without_state(), std::invalid_argument);
    CHECK_THROWS_AS(construct_without_register(), std::invalid_argument);
    CHECK_THROWS_AS(construct_without_unregister(), std::invalid_argument);
  }

  SECTION("first registration probes then confirms the fixed primary ID")
  {
    ScriptedHotkeyCalls api;
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      hotkey_detail::HotkeyRegistrationState{}, api.register_fn(),
      api.unregister_fn()};
    const auto chord = ctrl_chord(HotkeyKey::F12);

    const auto result = transaction.replace(chord);

    REQUIRE(result.has_value());
    REQUIRE(api.calls.size() == 3);
    CHECK(api.calls[0].kind == HotkeyCallKind::Register);
    CHECK(api.calls[0].id == hotkey_detail::probe_hotkey_id);
    CHECK(api.calls[1].kind == HotkeyCallKind::Unregister);
    CHECK(api.calls[1].id == hotkey_detail::probe_hotkey_id);
    CHECK(api.calls[2].kind == HotkeyCallKind::Register);
    CHECK(api.calls[2].id == hotkey_detail::primary_hotkey_id);
    CHECK(transaction.state().primary ==
          hotkey_detail::FixedHotkeyState::Registered);
    CHECK(transaction.state().probe == hotkey_detail::FixedHotkeyState::Free);
    CHECK(transaction.state().confirmed_chord == chord);
  }

  SECTION("successful registration clears stale cleanup diagnostics")
  {
    ScriptedHotkeyCalls api;
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      {.primary_cleanup_diagnostic =
         avb::core::AppError{.code =
                                     AppErrorCode::HotkeyCleanupFailure,
                                   .native_detail = 87},
       .probe_cleanup_diagnostic =
         avb::core::AppError{.code =
                                     AppErrorCode::HotkeyCleanupFailure,
                                   .native_detail = 87}},
      api.register_fn(),
      api.unregister_fn()};

    REQUIRE(transaction.replace(ctrl_chord(HotkeyKey::F12)).has_value());
    CHECK_FALSE(transaction.state().primary_cleanup_diagnostic.has_value());
    CHECK_FALSE(transaction.state().probe_cleanup_diagnostic.has_value());
  }

  SECTION("replacement is transactional and an equal confirmed chord is free")
  {
    const auto old_chord = ctrl_chord(HotkeyKey::F11);
    const auto new_chord = ctrl_chord(HotkeyKey::F12);
    ScriptedHotkeyCalls api;
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      confirmed_state(old_chord), api.register_fn(), api.unregister_fn()};

    REQUIRE(transaction.replace(new_chord).has_value());
    REQUIRE(api.calls.size() == 4);
    CHECK(api.calls[0].kind == HotkeyCallKind::Register);
    CHECK(api.calls[0].id == hotkey_detail::probe_hotkey_id);
    CHECK(api.calls[1].kind == HotkeyCallKind::Unregister);
    CHECK(api.calls[1].id == hotkey_detail::probe_hotkey_id);
    CHECK(api.calls[2].kind == HotkeyCallKind::Unregister);
    CHECK(api.calls[2].id == hotkey_detail::primary_hotkey_id);
    CHECK(api.calls[3].kind == HotkeyCallKind::Register);
    CHECK(api.calls[3].id == hotkey_detail::primary_hotkey_id);
    CHECK(transaction.state().confirmed_chord == new_chord);

    const auto call_count = api.calls.size();
    REQUIRE(transaction.replace(new_chord).has_value());
    CHECK(api.calls.size() == call_count);
  }

  SECTION("a probe conflict preserves the confirmed old primary")
  {
    const auto old_chord = ctrl_chord(HotkeyKey::F11);
    ScriptedHotkeyCalls api;
    api.register_results.push_back({.succeeded = false, .native_detail = 1409});
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      confirmed_state(old_chord), api.register_fn(), api.unregister_fn()};

    const auto result = transaction.replace(ctrl_chord(HotkeyKey::F12));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == AppErrorCode::HotkeyConflict);
    REQUIRE(api.calls.size() == 1);
    CHECK(api.calls.front().kind == HotkeyCallKind::Register);
    CHECK(api.calls.front().id == hotkey_detail::probe_hotkey_id);
    CHECK(transaction.state().primary ==
          hotkey_detail::FixedHotkeyState::Registered);
    CHECK(transaction.state().confirmed_chord == old_chord);
  }

  SECTION("a non-conflict registration failure remains a platform error")
  {
    ScriptedHotkeyCalls api;
    api.register_results.push_back(
      {.succeeded = false,
       .native_detail = ERROR_ACCESS_DENIED,
       .failure_kind =
         hotkey_detail::NativeRegistrationFailureKind::Platform});
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      hotkey_detail::HotkeyRegistrationState{}, api.register_fn(),
      api.unregister_fn()};

    const auto result = transaction.replace(ctrl_chord(HotkeyKey::F12));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == AppErrorCode::PlatformOperationFailure);
    CHECK(result.error().native_detail == ERROR_ACCESS_DENIED);
    REQUIRE(api.calls.size() == 1);
    CHECK(api.calls.front().kind == HotkeyCallKind::Register);
    CHECK(api.calls.front().id == hotkey_detail::probe_hotkey_id);
  }

  SECTION("an injected registration exception remains a platform error")
  {
    const hotkey_detail::RegisterHotkeyFn throwing_register =
      [](int,
         hotkey_detail::NativeHotkey) -> hotkey_detail::NativeCallResult {
      throw std::runtime_error{"injected registration failure"};
    };
    const hotkey_detail::UnregisterHotkeyFn unregister = [](int) {
      return hotkey_detail::NativeCallResult{.succeeded = true};
    };
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      hotkey_detail::HotkeyRegistrationState{}, throwing_register,
      unregister};

    const auto result = transaction.replace(ctrl_chord(HotkeyKey::F12));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == AppErrorCode::PlatformOperationFailure);
  }

  SECTION("probe cleanup failure preserves old primary and blocks ID reuse")
  {
    const auto old_chord = ctrl_chord(HotkeyKey::F11);
    ScriptedHotkeyCalls api;
    api.unregister_results.push_back({.succeeded = false, .native_detail = 87});
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      confirmed_state(old_chord), api.register_fn(), api.unregister_fn()};

    const auto first = transaction.replace(ctrl_chord(HotkeyKey::F12));

    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().code == AppErrorCode::HotkeyCleanupFailure);
    CHECK(transaction.state().probe ==
          hotkey_detail::FixedHotkeyState::Uncertain);
    CHECK(transaction.state().confirmed_chord == old_chord);
    REQUIRE(api.calls.size() == 2);

    api.calls.clear();
    api.unregister_results.push_back({.succeeded = false, .native_detail = 87});
    const auto second = transaction.replace(ctrl_chord(HotkeyKey::F10));

    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == AppErrorCode::HotkeyCleanupFailure);
    REQUIRE(api.calls.size() == 1);
    CHECK(api.calls.front().kind == HotkeyCallKind::Unregister);
    CHECK(api.calls.front().id == hotkey_detail::probe_hotkey_id);
  }

  SECTION(
    "lost primary becomes inactive and cleanup is retried before probing")
  {
    const auto old_chord = ctrl_chord(HotkeyKey::F11);
    ScriptedHotkeyCalls api;
    api.unregister_results.push_back({.succeeded = true});
    api.unregister_results.push_back({.succeeded = false, .native_detail = 87});
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      confirmed_state(old_chord), api.register_fn(), api.unregister_fn()};

    const auto first = transaction.replace(ctrl_chord(HotkeyKey::F12));

    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().code == AppErrorCode::HotkeyRegistrationLost);
    CHECK(transaction.state().primary ==
          hotkey_detail::FixedHotkeyState::Uncertain);
    CHECK_FALSE(transaction.state().confirmed_chord.has_value());
    REQUIRE(api.calls.size() == 3);
    CHECK(api.calls.back().kind == HotkeyCallKind::Unregister);
    CHECK(api.calls.back().id == hotkey_detail::primary_hotkey_id);

    api.calls.clear();
    api.unregister_results.push_back({.succeeded = false, .native_detail = 87});
    const auto second = transaction.replace(ctrl_chord(HotkeyKey::F10));

    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == AppErrorCode::HotkeyCleanupFailure);
    REQUIRE(api.calls.size() == 1);
    CHECK(api.calls.front().kind == HotkeyCallKind::Unregister);
    CHECK(api.calls.front().id == hotkey_detail::primary_hotkey_id);
  }

  SECTION("every uncertain ID is retried before replacement can continue")
  {
    ScriptedHotkeyCalls api;
    api.unregister_results.push_back({.succeeded = false, .native_detail = 87});
    api.unregister_results.push_back({.succeeded = true});
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      {.primary = hotkey_detail::FixedHotkeyState::Uncertain,
       .probe = hotkey_detail::FixedHotkeyState::Uncertain},
      api.register_fn(),
      api.unregister_fn()};

    const auto result = transaction.replace(ctrl_chord(HotkeyKey::F12));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == AppErrorCode::HotkeyCleanupFailure);
    REQUIRE(api.calls.size() == 2);
    CHECK(api.calls[0].kind == HotkeyCallKind::Unregister);
    CHECK(api.calls[0].id == hotkey_detail::primary_hotkey_id);
    CHECK(api.calls[1].kind == HotkeyCallKind::Unregister);
    CHECK(api.calls[1].id == hotkey_detail::probe_hotkey_id);
    CHECK(transaction.state().primary ==
          hotkey_detail::FixedHotkeyState::Uncertain);
    CHECK(transaction.state().probe == hotkey_detail::FixedHotkeyState::Free);
  }

  SECTION("a final registration race restores the old primary")
  {
    const auto old_chord = ctrl_chord(HotkeyKey::F11);
    ScriptedHotkeyCalls api;
    api.register_results.push_back({.succeeded = true});
    api.register_results.push_back({.succeeded = false, .native_detail = 1409});
    api.register_results.push_back({.succeeded = true});
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      confirmed_state(old_chord), api.register_fn(), api.unregister_fn()};

    const auto result = transaction.replace(ctrl_chord(HotkeyKey::F12));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == AppErrorCode::HotkeyConflict);
    REQUIRE(api.calls.size() == 5);
    CHECK(api.calls[3].kind == HotkeyCallKind::Register);
    CHECK(api.calls[4].kind == HotkeyCallKind::Register);
    CHECK(api.calls[3].hotkey != api.calls[4].hotkey);
    CHECK(transaction.state().primary ==
          hotkey_detail::FixedHotkeyState::Registered);
    CHECK(transaction.state().confirmed_chord == old_chord);
  }

  SECTION("rollback failure reports no active binding")
  {
    const auto old_chord = ctrl_chord(HotkeyKey::F11);
    ScriptedHotkeyCalls api;
    api.register_results.push_back({.succeeded = true});
    api.register_results.push_back({.succeeded = false, .native_detail = 1409});
    api.register_results.push_back({.succeeded = false, .native_detail = 1419});
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      confirmed_state(old_chord), api.register_fn(), api.unregister_fn()};

    const auto result = transaction.replace(ctrl_chord(HotkeyKey::F12));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == AppErrorCode::HotkeyRegistrationLost);
    CHECK(result.error().native_detail == 1419);
    REQUIRE(api.calls.size() == 5);
    CHECK(api.calls[0].kind == HotkeyCallKind::Register);
    CHECK(api.calls[0].id == hotkey_detail::probe_hotkey_id);
    CHECK(api.calls[1].kind == HotkeyCallKind::Unregister);
    CHECK(api.calls[1].id == hotkey_detail::probe_hotkey_id);
    CHECK(api.calls[2].kind == HotkeyCallKind::Unregister);
    CHECK(api.calls[2].id == hotkey_detail::primary_hotkey_id);
    REQUIRE(api.calls[3].hotkey.has_value());
    CHECK(api.calls[3].kind == HotkeyCallKind::Register);
    CHECK(api.calls[3].id == hotkey_detail::primary_hotkey_id);
    CHECK(api.calls[3].hotkey ==
          hotkey_detail::map_hotkey_to_native(ctrl_chord(HotkeyKey::F12)));
    REQUIRE(api.calls[4].hotkey.has_value());
    CHECK(api.calls[4].kind == HotkeyCallKind::Register);
    CHECK(api.calls[4].id == hotkey_detail::primary_hotkey_id);
    CHECK(api.calls[4].hotkey ==
          hotkey_detail::map_hotkey_to_native(old_chord));
    CHECK(transaction.state().primary == hotkey_detail::FixedHotkeyState::Free);
    CHECK_FALSE(transaction.state().confirmed_chord.has_value());
  }

  SECTION("transaction operations reject the wrong thread before mutation")
  {
    const auto chord = ctrl_chord(HotkeyKey::F12);
    ScriptedHotkeyCalls replace_api;
    hotkey_detail::HotkeyRegistrationTransaction replace_transaction{
      hotkey_detail::HotkeyRegistrationState{}, replace_api.register_fn(),
      replace_api.unregister_fn()};
    std::optional<std::expected<void, avb::core::AppError>> result;

    std::jthread replace_thread{
      [&] { result = replace_transaction.replace(chord); }};
    replace_thread.join();

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    CHECK(result->error().code == AppErrorCode::PlatformOperationFailure);
    CHECK(result->error().native_detail == ERROR_INVALID_THREAD_ID);
    CHECK(replace_api.calls.empty());
    CHECK(replace_transaction.state().primary ==
          hotkey_detail::FixedHotkeyState::Free);
    CHECK_FALSE(replace_transaction.state().confirmed_chord.has_value());

    ScriptedHotkeyCalls clear_api;
    hotkey_detail::HotkeyRegistrationTransaction clear_transaction{
      confirmed_state(chord), clear_api.register_fn(),
      clear_api.unregister_fn()};
    std::jthread clear_thread{[&] { clear_transaction.clear(); }};
    clear_thread.join();

    CHECK(clear_api.calls.empty());
    CHECK(clear_transaction.state().primary ==
          hotkey_detail::FixedHotkeyState::Registered);
    CHECK(clear_transaction.state().confirmed_chord == chord);
  }

  SECTION("public replacement rejects the wrong owner thread")
  {
    const HotkeyChordSink sink = [](HotkeyChord) {};
    auto hotkey =
      avb::platform::windows::WindowsGlobalHotkey::create(sink);
    REQUIRE(hotkey.has_value());
    std::optional<std::expected<void, avb::core::AppError>> result;

    std::jthread worker{
      [&] { result = (*hotkey)->replace(ctrl_chord(HotkeyKey::F12)); }};
    worker.join();

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    CHECK(result->error().code == AppErrorCode::PlatformOperationFailure);
    CHECK(result->error().native_detail == ERROR_INVALID_THREAD_ID);
  }

  SECTION("clear attempts both IDs and retains cleanup diagnostics")
  {
    const auto old_chord = ctrl_chord(HotkeyKey::F11);
    ScriptedHotkeyCalls api;
    api.unregister_results.push_back({.succeeded = false, .native_detail = 87});
    api.unregister_results.push_back({.succeeded = true});
    hotkey_detail::HotkeyRegistrationTransaction transaction{
      confirmed_state(old_chord), api.register_fn(), api.unregister_fn()};

    transaction.clear();

    REQUIRE(api.calls.size() == 2);
    CHECK(api.calls[0].id == hotkey_detail::primary_hotkey_id);
    CHECK(api.calls[1].id == hotkey_detail::probe_hotkey_id);
    CHECK(transaction.state().primary ==
          hotkey_detail::FixedHotkeyState::Uncertain);
    CHECK(transaction.state().probe == hotkey_detail::FixedHotkeyState::Free);
    CHECK_FALSE(transaction.state().confirmed_chord.has_value());
    REQUIRE(transaction.state().primary_cleanup_diagnostic.has_value());
    CHECK(transaction.state().primary_cleanup_diagnostic->native_detail == 87);
    CHECK_FALSE(transaction.state().probe_cleanup_diagnostic.has_value());
  }

  SECTION("the production WndProc ignores unsafe and stale messages")
  {
    const auto chord = ctrl_chord(HotkeyKey::F12);
    std::vector<HotkeyChord> chords;
    const HotkeyChordSink sink = [&chords](HotkeyChord received) {
      chords.push_back(received);
    };
    const auto dispatch_state =
      std::make_shared<hotkey_detail::HotkeyDispatchState>(
        confirmed_state(chord), sink);
    auto window = hotkey_detail::HotkeyMessageWindow::create(dispatch_state);
    REQUIRE(window.has_value());
    const auto matching_message = hotkey_message_parameter(chord);

    SendMessageW(window->native_handle(), WM_HOTKEY,
                 hotkey_detail::probe_hotkey_id, matching_message);
    SendMessageW(window->native_handle(), WM_HOTKEY, 999, matching_message);
    auto uncertain_state = confirmed_state(chord);
    uncertain_state.primary = hotkey_detail::FixedHotkeyState::Uncertain;
    const auto uncertain_dispatch =
      std::make_shared<hotkey_detail::HotkeyDispatchState>(
        std::move(uncertain_state), sink);
    auto uncertain_window =
      hotkey_detail::HotkeyMessageWindow::create(uncertain_dispatch);
    REQUIRE(uncertain_window.has_value());
    SendMessageW(uncertain_window->native_handle(), WM_HOTKEY,
                 hotkey_detail::primary_hotkey_id, matching_message);
    CHECK(chords.empty());

    SendMessageW(window->native_handle(), WM_HOTKEY,
                 hotkey_detail::primary_hotkey_id,
                 hotkey_message_parameter(ctrl_chord(HotkeyKey::F11)));
    CHECK(chords.empty());

    SendMessageW(window->native_handle(), WM_HOTKEY,
                 hotkey_detail::primary_hotkey_id, matching_message);
    REQUIRE(chords.size() == 1);
    CHECK(chords.front() == chord);
    CHECK(dispatch_state->registration_state().confirmed_chord == chord);
    CHECK(dispatch_state->registration_state().primary ==
          hotkey_detail::FixedHotkeyState::Registered);
  }

  SECTION("the production WndProc contains a throwing stable chord sink")
  {
    const auto state = confirmed_state(ctrl_chord(HotkeyKey::F12));
    std::size_t attempts{};
    std::vector<HotkeyChord> chords;
    const HotkeyChordSink sink = [&attempts, &chords](HotkeyChord received) {
      ++attempts;
      if (attempts == 1) {
        throw std::bad_alloc{};
      }
      chords.push_back(received);
    };
    const auto dispatch_state =
      std::make_shared<hotkey_detail::HotkeyDispatchState>(state, sink);
    auto window = hotkey_detail::HotkeyMessageWindow::create(dispatch_state);
    REQUIRE(window.has_value());
    const auto matching_message =
      hotkey_message_parameter(ctrl_chord(HotkeyKey::F12));

    SendMessageW(window->native_handle(), WM_HOTKEY,
                 hotkey_detail::primary_hotkey_id, matching_message);
    SendMessageW(window->native_handle(), WM_HOTKEY,
                 hotkey_detail::primary_hotkey_id, matching_message);

    CHECK(attempts == 2);
    REQUIRE(chords.size() == 1);
    CHECK(chords.front() == state.confirmed_chord);
  }

  SECTION("the production WndProc survives reentrant window destruction")
  {
    const auto chord = ctrl_chord(HotkeyKey::F12);
    std::optional<hotkey_detail::HotkeyMessageWindow> owned_window;
    std::weak_ptr<hotkey_detail::HotkeyDispatchState> dispatch_lifetime;
    std::size_t emitted{};
    bool chord_matched{};
    bool dispatch_retained_after_reset{};
    const HotkeyChordSink sink =
      [&owned_window, &dispatch_lifetime, &emitted, &chord_matched,
       &dispatch_retained_after_reset, chord](HotkeyChord received) {
        auto *const window = &owned_window;
        auto *const lifetime = &dispatch_lifetime;
        auto *const emitted_count = &emitted;
        auto *const chord_observation = &chord_matched;
        auto *const retained_observation = &dispatch_retained_after_reset;

        ++*emitted_count;
        *chord_observation = received == chord;
        window->reset();
        *retained_observation = !lifetime->expired();
      };
    auto dispatch_state = std::make_shared<hotkey_detail::HotkeyDispatchState>(
      confirmed_state(chord), sink);
    dispatch_lifetime = dispatch_state;
    auto created = hotkey_detail::HotkeyMessageWindow::create(dispatch_state);
    REQUIRE(created.has_value());
    owned_window.emplace(std::move(*created));
    const auto native_window = owned_window->native_handle();
    dispatch_state.reset();

    const auto result =
      SendMessageW(native_window, WM_HOTKEY, hotkey_detail::primary_hotkey_id,
                   hotkey_message_parameter(chord));

    CHECK(result == 0);
    CHECK(emitted == 1);
    CHECK(chord_matched);
    CHECK(dispatch_retained_after_reset);
    CHECK_FALSE(owned_window.has_value());
    CHECK(dispatch_lifetime.expired());
  }
}

TEST_CASE(
  "avb0795 V1 case 12: Windows capture callback and frame ownership "
  "contracts",
  "[avb0795-v1]")
{
  using avb::core::AppErrorCode;
  using avb::core::Extent;
  using avb::platform::windows::CaptureFatalCategory;

  constexpr auto format = PixelFormat::B8G8R8A8UIntNormalized;
  constexpr Extent allocation{.width = 640, .height = 480};

  std::array<wchar_t, 2> destruction_child_mode{};
  if (GetEnvironmentVariableW(destruction_invariant_child_environment,
                              destruction_child_mode.data(),
                              destruction_child_mode.size()) != 0) {
    std::set_terminate(
      [] noexcept { ExitProcess(destruction_invariant_exit_code); });
    capture_detail::CaptureJobStopState failed_stop;
    failed_stop.request_stop([] { throw winrt::hresult_error{E_ABORT}; },
                             [] {});
    capture_detail::enforce_wgc_job_destruction_invariant(
      failed_stop.stop_quiescent(true, true));
  }

  SECTION("observation and source wake lifetime")
  {
    auto controlled = winrt::make_self<ControlledCaptureItem>();
    auto item =
      controlled.as<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
    auto observation = capture_detail::ItemObservationState::create(item);
    CHECK(controlled->registration_count == 1);

    controlled->fire();
    WindowsSourceRegistry registry;
    auto refresh = registry.begin_refresh();
    REQUIRE(refresh.has_value());
    std::vector<std::shared_ptr<capture_detail::ItemObservationState>> entries;
    entries.push_back(observation);
    auto ids = refresh->commit(std::move(entries));
    REQUIRE(ids.has_value());
    REQUIRE(ids->size() == 1);
    auto lease = registry.resolve(ids->front());
    REQUIRE(lease.has_value());

    std::uint32_t wake_count{};
    auto source = WindowsCaptureSource::create(std::move(*lease),
                                               [&wake_count] { ++wake_count; });
    REQUIRE(source.has_value());
    CHECK((*source)->is_closed());
    CHECK((*source)->consume_closed_edge());
    CHECK_FALSE((*source)->consume_closed_edge());
    CHECK(wake_count == 0);

    for (const auto generation : std::array<std::uint64_t, 2>{7, 8}) {
      capture_detail::FrameCallbackPolicy replacement_policy{
        3, allocation, generation, format, [] {}};
      replacement_policy.open_current_epoch();
      CHECK(replacement_policy.admission_open());
      CHECK(controlled->registration_count == 1);
    }

    {
      auto replacement_lease = registry.resolve(ids->front());
      REQUIRE(replacement_lease.has_value());
      CHECK(controlled->registration_count == 1);
    }

    (*source)->request_stop();
    CHECK((*source)->callbacks_quiescent());
    controlled->fire();
    CHECK(wake_count == 0);
    source->reset();

    auto replacement_lease = registry.resolve(ids->front());
    REQUIRE(replacement_lease.has_value());
    auto replacement_source = WindowsCaptureSource::create(
      std::move(*replacement_lease), [&wake_count] { ++wake_count; });
    REQUIRE(replacement_source.has_value());
    CHECK((*replacement_source)->is_closed());
    CHECK((*replacement_source)->consume_closed_edge());
    CHECK(controlled->registration_count == 1);
    replacement_source->reset();
    observation.reset();
    CHECK(controlled->revocation_count == 0);

    {
      auto unpublished_item = winrt::make_self<ControlledCaptureItem>();
      auto unpublished_observation = capture_detail::ItemObservationState::create(
        unpublished_item
          .as<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>());
      WindowsSourceRegistry unpublished_registry;
      auto unpublished_refresh = unpublished_registry.begin_refresh();
      REQUIRE(unpublished_refresh.has_value());
      std::vector<std::shared_ptr<capture_detail::ItemObservationState>>
        unpublished_entries{unpublished_observation};
      auto unpublished_ids =
        unpublished_refresh->commit(std::move(unpublished_entries));
      REQUIRE(unpublished_ids.has_value());
      auto unpublished_lease =
        unpublished_registry.resolve(unpublished_ids->front());
      REQUIRE(unpublished_lease.has_value());
      std::uint32_t unpublished_wakes{};
      auto unpublished_source = WindowsCaptureSource::create(
        std::move(*unpublished_lease), [&] { ++unpublished_wakes; });
      REQUIRE(unpublished_source.has_value());
      unpublished_item->fire();
      CHECK((*unpublished_source)->is_closed());
      CHECK((*unpublished_source)->consume_closed_edge());
      CHECK(unpublished_wakes == 0);
      CHECK((*unpublished_source)->callbacks_quiescent());
    }

    auto active_controlled = winrt::make_self<ControlledCaptureItem>();
    auto active_observation = capture_detail::ItemObservationState::create(
      active_controlled
        .as<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>());
    std::binary_semaphore wake_entered{0};
    std::binary_semaphore wake_release{0};
    std::uint32_t active_wakes{};
    {
      WindowsSourceRegistry active_registry;
      auto active_refresh = active_registry.begin_refresh();
      REQUIRE(active_refresh.has_value());
      std::vector<std::shared_ptr<capture_detail::ItemObservationState>>
        active_entries;
      active_entries.push_back(active_observation);
      auto active_ids = active_refresh->commit(std::move(active_entries));
      REQUIRE(active_ids.has_value());
      auto active_lease = active_registry.resolve(active_ids->front());
      REQUIRE(active_lease.has_value());
      auto active_source =
        WindowsCaptureSource::create(std::move(*active_lease), [&] {
          ++active_wakes;
          if (active_wakes == 1) {
            wake_entered.release();
            wake_release.acquire();
          }
        });
      REQUIRE(active_source.has_value());
      (*active_source)->activate_runtime_wake();

      std::jthread close_callback{[&] { active_controlled->fire(); }};
      wake_entered.acquire();
      (*active_source)->request_stop();
      CHECK_FALSE((*active_source)->callbacks_quiescent());
      wake_release.release();
      close_callback.join();
      CHECK((*active_source)->callbacks_quiescent());
      CHECK((*active_source)->is_closed());
      CHECK((*active_source)->consume_closed_edge());
      active_controlled->fire();
      CHECK(active_wakes == 2);
      active_source->reset();
    }
    CHECK(active_controlled->revocation_count == 1);
    active_controlled->fire_late();
    CHECK(active_wakes == 2);

    std::binary_semaphore admitted_wake_entered{0};
    std::binary_semaphore admitted_wake_release{0};
    std::binary_semaphore final_wake_entered{0};
    std::binary_semaphore final_wake_release{0};
    std::uint32_t linearized_wakes{};
    capture_detail::RuntimeWakeState admission_wins{[&] {
      ++linearized_wakes;
      if (linearized_wakes == 1) {
        admitted_wake_entered.release();
        admitted_wake_release.acquire();
      }
      else {
        final_wake_entered.release();
        final_wake_release.acquire();
        throw winrt::hresult_error{E_ABORT};
      }
    }};
    std::jthread admitted_callback{[&] { admission_wins.wake(); }};
    admitted_wake_entered.acquire();
    admission_wins.request_stop();
    CHECK_FALSE(admission_wins.callbacks_quiescent());
    admitted_wake_release.release();
    final_wake_entered.acquire();
    CHECK_FALSE(admission_wins.callbacks_quiescent());
    final_wake_release.release();
    admitted_callback.join();
    CHECK(admission_wins.callbacks_quiescent());
    CHECK(linearized_wakes == 2);
    admission_wins.wake();
    CHECK(linearized_wakes == 2);

    std::uint32_t stop_wins_wakes{};
    capture_detail::RuntimeWakeState stop_wins{[&] { ++stop_wins_wakes; }};
    stop_wins.request_stop();
    CHECK(stop_wins.callbacks_quiescent());
    stop_wins.wake();
    CHECK(stop_wins.callbacks_quiescent());
    CHECK(stop_wins_wakes == 0);
  }

  SECTION("publication and both pending arrival handshake interleavings")
  {
    std::uint32_t callback_first_wakes{};
    capture_detail::FrameCallbackPolicy callback_first{
      3, allocation, 7, format,
      [&callback_first_wakes] { ++callback_first_wakes; }};
    auto callback_first_admission =
      callback_first.enter(callback_first.current_epoch());
    REQUIRE(callback_first_admission.has_value());
    CHECK_FALSE(callback_first_admission->is_open());
    callback_first_admission->record_pending();
    callback_first.open_current_epoch();
    callback_first_admission->wake_if_open();
    callback_first_admission.reset();
    CHECK(callback_first.arrival_pending());
    CHECK(callback_first_wakes >= 1);
    CHECK(callback_first.claim_pending_arrival());
    CHECK_FALSE(callback_first.claim_pending_arrival());

    std::uint32_t opener_first_wakes{};
    capture_detail::FrameCallbackPolicy opener_first{
      3, allocation, 7, format,
      [&opener_first_wakes] { ++opener_first_wakes; }};
    auto opener_first_admission =
      opener_first.enter(opener_first.current_epoch());
    REQUIRE(opener_first_admission.has_value());
    CHECK_FALSE(opener_first_admission->is_open());
    opener_first.open_current_epoch();
    opener_first_admission->record_pending();
    opener_first_admission->wake_if_open();
    opener_first_admission.reset();
    CHECK(opener_first.arrival_pending());
    CHECK(opener_first_wakes >= 1);
    CHECK(opener_first.claim_pending_arrival());
    CHECK_FALSE(opener_first.claim_pending_arrival());

    constexpr std::size_t concurrent_handshake_iterations = 20'000;
    std::barrier handshake_phase{3};
    std::unique_ptr<capture_detail::FrameCallbackPolicy> concurrent_policy;
    std::unique_ptr<capture_detail::FrameCallbackPolicy::Admission>
      concurrent_admission;
    std::atomic_uint32_t concurrent_wakes{};
    std::uint32_t missed_handshakes{};
    std::jthread concurrent_callback{[&] {
      for (std::size_t iteration = 0;
           iteration < concurrent_handshake_iterations; ++iteration) {
        handshake_phase.arrive_and_wait();
        concurrent_admission->record_pending();
        concurrent_admission->wake_if_open();
        handshake_phase.arrive_and_wait();
      }
    }};
    std::jthread concurrent_opener{[&] {
      for (std::size_t iteration = 0;
           iteration < concurrent_handshake_iterations; ++iteration) {
        handshake_phase.arrive_and_wait();
        concurrent_policy->open_current_epoch();
        handshake_phase.arrive_and_wait();
      }
    }};
    for (std::size_t iteration = 0; iteration < concurrent_handshake_iterations;
         ++iteration) {
      concurrent_wakes.store(0, std::memory_order_relaxed);
      concurrent_policy = std::make_unique<capture_detail::FrameCallbackPolicy>(
        3, allocation, 7, format,
        [&] { concurrent_wakes.fetch_add(1, std::memory_order_relaxed); });
      auto entered =
        concurrent_policy->enter(concurrent_policy->current_epoch());
      concurrent_admission =
        std::make_unique<capture_detail::FrameCallbackPolicy::Admission>(
          std::move(entered.value()));
      handshake_phase.arrive_and_wait();
      handshake_phase.arrive_and_wait();
      if (concurrent_wakes.load(std::memory_order_relaxed) == 0) {
        ++missed_handshakes;
      }
      concurrent_admission.reset();
      concurrent_policy.reset();
    }
    concurrent_callback.join();
    concurrent_opener.join();
    CHECK(missed_handshakes == 0);

    capture_detail::FrameCallbackPolicy pending_publication{3, allocation, 7,
                                                            format, [] {}};
    auto pending_admission =
      pending_publication.enter(pending_publication.current_epoch());
    REQUIRE(pending_admission.has_value());
    CHECK_FALSE(pending_admission->is_open());
    pending_admission->record_pending();
    pending_admission->wake_if_open();
    pending_admission.reset();
    CHECK(pending_publication.arrival_pending());
    capture_detail::PublicationGate pending_gate;
    REQUIRE(pending_gate
              .start_for_publication(
                [] { return false; }, [] {},
                [&] { pending_publication.open_current_epoch(); })
              .has_value());
    CHECK(pending_publication.claim_pending_arrival());
    CHECK_FALSE(pending_publication.arrival_pending());
    CHECK(pending_publication.pool_buffer_count() == 3);

    capture_detail::PublicationGate gate;
    std::vector<int> order;
    bool source_closed{};
    auto published = gate.start_for_publication(
      [&] {
        order.push_back(1);
        return source_closed;
      },
      [&] { order.push_back(2); }, [&] { order.push_back(3); });
    REQUIRE(published.has_value());
    CHECK(order == std::vector{1, 2, 3});
    const auto repeated =
      gate.start_for_publication([] { return false; }, [] {}, [] {});
    REQUIRE_FALSE(repeated.has_value());
    CHECK(repeated.error().code == AppErrorCode::InvalidCommand);

    auto post_check_item = winrt::make_self<ControlledCaptureItem>();
    auto post_check_observation = capture_detail::ItemObservationState::create(
      post_check_item
        .as<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>());
    WindowsSourceRegistry post_check_registry;
    auto post_check_refresh = post_check_registry.begin_refresh();
    REQUIRE(post_check_refresh.has_value());
    std::vector<std::shared_ptr<capture_detail::ItemObservationState>>
      post_check_entries;
    post_check_entries.push_back(post_check_observation);
    auto post_check_ids =
      post_check_refresh->commit(std::move(post_check_entries));
    REQUIRE(post_check_ids.has_value());
    auto post_check_lease =
      post_check_registry.resolve(post_check_ids->front());
    REQUIRE(post_check_lease.has_value());
    auto post_check_source =
      WindowsCaptureSource::create(std::move(*post_check_lease), [] {});
    REQUIRE(post_check_source.has_value());
    capture_detail::PublicationGate post_check_gate;
    const auto post_check_started = post_check_gate.start_for_publication(
      [&] { return (*post_check_source)->is_closed(); },
      [&] { post_check_item->fire(); }, [] {});
    REQUIRE(post_check_started.has_value());
    (*post_check_source)->activate_runtime_wake();
    CHECK((*post_check_source)->is_closed());
    CHECK((*post_check_source)->consume_closed_edge());
    CHECK_FALSE((*post_check_source)->consume_closed_edge());
    CHECK(post_check_observation->closed_sequence() == 1);
    (*post_check_source)->request_stop();

    struct UnpublishedAggregate final {
      UnpublishedAggregate(const Extent extent, const PixelFormat pixel_format, bool &destroyed)
          : frame_policy{2, extent, 7, pixel_format, [] {}},
            destroyed_{destroyed} {}
      ~UnpublishedAggregate() { destroyed_ = true; }

      capture_detail::FrameCallbackPolicy frame_policy;
      bool &destroyed_;
    };

    bool unpublished_destroyed{};
    auto unpublished = std::make_unique<UnpublishedAggregate>(
      allocation, format, unpublished_destroyed);
    auto unpublished_admission = unpublished->frame_policy.enter(
      unpublished->frame_policy.current_epoch());
    REQUIRE(unpublished_admission.has_value());
    CHECK_FALSE(unpublished_admission->is_open());
    unpublished_admission->record_pending();
    unpublished_admission.reset();
    CHECK(unpublished->frame_policy.arrival_pending());
    capture_detail::PublicationGate failed_gate;
    const auto failed = failed_gate.start_for_publication(
      [] { return false; },
      [] { throw winrt::hresult_error{E_ACCESSDENIED}; },
      [&] { unpublished->frame_policy.open_current_epoch(); });
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == AppErrorCode::PermissionDenied);
    CHECK_FALSE(unpublished_destroyed);
    unpublished.reset();
    CHECK(unpublished_destroyed);

    constexpr std::array construction_rows{
      std::pair{true, AppErrorCode::SourceUnavailable},
      std::pair{false, AppErrorCode::RuntimeConstructionFailure},
    };
    for (const auto &[source_is_closed, expected] : construction_rows) {
      const auto classified = capture_detail::classify_wgc_construction_failure(
        source_is_closed, E_ABORT);
      CHECK(classified.code == expected);
      CHECK(classified.native_detail == E_ABORT);
    }
    const auto permission = capture_detail::classify_wgc_construction_failure(
      false, E_ACCESSDENIED, true);
    CHECK(permission.code == AppErrorCode::PermissionDenied);

    capture_detail::PublicationGate closed_gate;
    bool start_called{};
    const auto closed = closed_gate.start_for_publication(
      [] { return true; }, [&] { start_called = true; }, [] {});
    REQUIRE_FALSE(closed.has_value());
    CHECK(closed.error().code == AppErrorCode::SourceUnavailable);
    CHECK_FALSE(start_called);
  }

  SECTION("callback containment and immutable format validation")
  {
    struct Row final {
      int kind;
      CaptureFatalCategory expected;
      std::int64_t native_detail;
    };
    constexpr std::array rows{
      Row{0, CaptureFatalCategory::ProjectionFailure, E_ABORT},
      Row{1, CaptureFatalCategory::OutOfMemory, E_OUTOFMEMORY},
      Row{2, CaptureFatalCategory::UnknownCallbackFailure, E_FAIL},
      Row{3, CaptureFatalCategory::FormatMismatch, 0},
      Row{4, CaptureFatalCategory::InvalidFrameMetadata, E_INVALIDARG},
      Row{5, CaptureFatalCategory::InvalidFrameMetadata, E_INVALIDARG},
      Row{6, CaptureFatalCategory::InvalidFrameMetadata, E_INVALIDARG},
    };

    for (const auto &row : rows) {
      CAPTURE(row.kind);
      std::uint32_t wakes{};
      capture_detail::FrameCallbackPolicy policy{3, allocation, 7, format,
                                                 [&] { ++wakes; }};
      policy.open_current_epoch();
      auto admission = policy.enter(policy.current_epoch());
      REQUIRE(admission.has_value());
      REQUIRE(admission->is_open());
      auto close_count = std::make_shared<std::uint32_t>();
      if (row.kind <= 1) {
        WindowsCapturedFrame acquired{
          1,
          7,
          allocation,
          allocation,
          format,
          std::chrono::steady_clock::time_point{},
          make_controlled_capture_frame(close_count)};
        if (row.kind == 0) {
          policy.record_projection_failure(E_ABORT);
        }
        else {
          policy.record_out_of_memory();
        }
      }
      else if (row.kind == 2) {
        policy.record_unknown_callback_failure();
      }
      else {
        auto observed = capture_detail::FrameObservation{
          .resource_generation = 7,
          .allocation_extent = allocation,
          .content_size = allocation,
          .surface_format = format,
        };
        if (row.kind == 3) {
          observed.surface_format = PixelFormat::R16G16B16A16Float;
        }
        else if (row.kind == 4) {
          observed.allocation_extent = {.width = 800, .height = 600};
        }
        else if (row.kind == 5) {
          observed.content_size = {.width = 0, .height = 480};
        }
        else {
          observed.content_size = {
            .width = (std::numeric_limits<std::uint32_t>::max)(),
            .height = 480};
        }
        CHECK_FALSE(policy.observe_frame(observed).accepted);
      }
      admission.reset();

      const auto first = policy.drain_signals();
      CHECK(first.fatal_active);
      CHECK(first.fatal_edge);
      CHECK(first.fatal.category == row.expected);
      CHECK(first.fatal.native_detail == row.native_detail);
      CHECK(wakes == 1);
      CHECK(policy.frame_callbacks_quiescent());
      if (row.kind <= 1) {
        CHECK(*close_count == 1);
      }
      const auto second = policy.drain_signals();
      CHECK(second.fatal_active);
      CHECK_FALSE(second.fatal_edge);
    }
  }

  SECTION("stop and reconfigure remain nonblocking and quiescent")
  {
    capture_detail::enforce_wgc_job_destruction_invariant(true);
    const auto invariant_child_exit = run_destruction_invariant_child();
    REQUIRE(invariant_child_exit.has_value());
    CHECK(*invariant_child_exit == destruction_invariant_exit_code);

    std::binary_semaphore closed_entered{0};
    std::binary_semaphore closed_release{0};
    std::uint32_t unopened_wakes{};
    capture_detail::FrameCallbackPolicy closed_policy{
      3, allocation, 7, format, [&] { ++unopened_wakes; }};
    std::jthread closed_callback{[&] {
      auto admission = closed_policy.enter(closed_policy.current_epoch());
      closed_entered.release();
      closed_release.acquire();
      if (admission && !admission->is_open()) {
        admission->record_pending();
        admission->wake_if_open();
      }
    }};
    closed_entered.acquire();
    CHECK_FALSE(closed_policy.frame_callbacks_quiescent());
    closed_release.release();
    closed_callback.join();
    CHECK(closed_policy.frame_callbacks_quiescent());
    CHECK(closed_policy.arrival_pending());

    auto unopened_admission = closed_policy.enter(closed_policy.current_epoch());
    REQUIRE(unopened_admission.has_value());
    closed_policy.request_terminal_stop();
    CHECK_FALSE(closed_policy.frame_callbacks_quiescent());
    unopened_admission.reset();
    CHECK(closed_policy.frame_callbacks_quiescent());
    CHECK(unopened_wakes == 0);

    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    std::binary_semaphore frame_final_wake_entered{0};
    std::binary_semaphore frame_final_wake_release{0};
    std::uint32_t frame_quiescence_wakes{};
    capture_detail::FrameCallbackPolicy state{
      3, allocation, 7, format, [&] {
        ++frame_quiescence_wakes;
        frame_final_wake_entered.release();
        frame_final_wake_release.acquire();
      }};
    state.open_current_epoch();
    const auto old_epoch = state.current_epoch();
    std::jthread callback{[&] {
      auto admission = state.enter(old_epoch);
      entered.release();
      release.acquire();
    }};
    entered.acquire();
    const auto prepared_epoch = state.close_for_reconfigure();
    CHECK(prepared_epoch != old_epoch);
    CHECK_FALSE(state.frame_callbacks_quiescent());
    release.release();
    frame_final_wake_entered.acquire();
    CHECK_FALSE(state.frame_callbacks_quiescent());
    CHECK(frame_quiescence_wakes == 1);
    frame_final_wake_release.release();
    callback.join();
    CHECK(state.frame_callbacks_quiescent());
    CHECK(frame_quiescence_wakes == 1);

    CHECK_FALSE(state.enter(old_epoch).has_value());
    REQUIRE(state.prepare_current_epoch({.width = 800, .height = 600}, 8)
              .has_value());
    CHECK_FALSE(state.admission_open());
    state.open_current_epoch();
    CHECK(state.admission_open());

    constexpr Extent reconfigured_allocation{.width = 800, .height = 600};
    for (const auto callback_first : std::array{true, false}) {
      CAPTURE(callback_first);
      std::uint32_t handshake_wakes{};
      capture_detail::FrameCallbackPolicy current_epoch_state{
        3, allocation, 7, format, [&handshake_wakes] { ++handshake_wakes; }};
      current_epoch_state.open_current_epoch();
      static_cast<void>(current_epoch_state.close_for_reconfigure());
      REQUIRE(
        current_epoch_state.prepare_current_epoch(reconfigured_allocation, 8)
          .has_value());
      CHECK_FALSE(current_epoch_state.admission_open());

      auto admission =
        current_epoch_state.enter(current_epoch_state.current_epoch());
      REQUIRE(admission.has_value());
      CHECK_FALSE(admission->is_open());
      if (callback_first) {
        admission->record_pending();
        current_epoch_state.open_current_epoch();
        admission->wake_if_open();
      }
      else {
        current_epoch_state.open_current_epoch();
        admission->record_pending();
        admission->wake_if_open();
      }
      admission.reset();
      CHECK(current_epoch_state.arrival_pending());
      CHECK(current_epoch_state.admission_open());
      CHECK(handshake_wakes >= 1);
      CHECK(current_epoch_state.pool_buffer_count() == 3);
      CHECK(current_epoch_state.claim_pending_arrival());
      CHECK_FALSE(current_epoch_state.arrival_pending());
      CHECK_FALSE(current_epoch_state.claim_pending_arrival());
    }

    auto final_check_item = winrt::make_self<ControlledCaptureItem>();
    auto final_check_observation = capture_detail::ItemObservationState::create(
      final_check_item
        .as<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>());
    capture_detail::FrameCallbackPolicy final_check_state{3, allocation, 7,
                                                          format, [] {}};
    final_check_state.open_current_epoch();
    static_cast<void>(final_check_state.close_for_reconfigure());
    REQUIRE(final_check_state.prepare_current_epoch(reconfigured_allocation, 8)
              .has_value());
    final_check_item->fire();
    CHECK(final_check_observation->is_closed());
    CHECK_FALSE(final_check_state.admission_open());
    CHECK(
      capture_detail::classify_wgc_construction_failure(true, E_ABORT).code ==
      AppErrorCode::SourceUnavailable);
    final_check_state.request_terminal_stop();
    CHECK_FALSE(final_check_state.arrival_pending());

    capture_detail::FrameCallbackPolicy bounded_state{3, allocation, 7, format,
                                                      [] {}};
    auto bounded_admission = bounded_state.enter(bounded_state.current_epoch());
    REQUIRE(bounded_admission.has_value());
    bounded_admission->record_pending();
    bounded_admission.reset();
    bounded_state.open_current_epoch();
    CHECK(bounded_state.claim_pending_arrival());
    CHECK(bounded_state.pool_buffer_count() == 3);

    state.request_terminal_stop();
    using CleanupCategory =
      avb::platform::windows::CaptureCleanupCategory;
    for (const auto revoke_fails : std::array{true, false}) {
      capture_detail::CaptureJobStopState stop_state;
      stop_state.request_stop(
        [&] {
          if (revoke_fails) {
            throw winrt::hresult_error{E_ABORT};
          }
        },
        [&] {
          if (!revoke_fails) {
            throw winrt::hresult_error{E_ACCESSDENIED};
          }
        });
      CHECK_FALSE(stop_state.native_stop_succeeded());
      REQUIRE(stop_state.cleanup_failure().has_value());
      CHECK(stop_state.cleanup_failure()->category ==
            (revoke_fails ? CleanupCategory::FrameArrivedRevocationFailure
                          : CleanupCategory::CaptureSessionCloseFailure));
      CHECK(stop_state.cleanup_failure()->native_detail ==
            (revoke_fails ? E_ABORT : E_ACCESSDENIED));
      CHECK_FALSE(
        stop_state.stop_quiescent(state.frame_callbacks_quiescent(), true));
      stop_state.request_stop([] {}, [] {});
      CHECK_FALSE(stop_state.native_stop_succeeded());
    }

    std::binary_semaphore stop_entered{0};
    std::binary_semaphore stop_release{0};
    std::binary_semaphore terminal_final_wake_entered{0};
    std::binary_semaphore terminal_final_wake_release{0};
    std::uint32_t terminal_quiescence_wakes{};
    capture_detail::FrameCallbackPolicy terminal_state{
      3, allocation, 7, format, [&] {
        ++terminal_quiescence_wakes;
        terminal_final_wake_entered.release();
        terminal_final_wake_release.acquire();
        throw winrt::hresult_error{E_ABORT};
      }};
    terminal_state.open_current_epoch();
    std::jthread terminal_callback{[&] {
      auto admission = terminal_state.enter(terminal_state.current_epoch());
      stop_entered.release();
      stop_release.acquire();
    }};
    stop_entered.acquire();
    terminal_state.request_terminal_stop();
    capture_detail::CaptureJobStopState terminal_stop_state;
    terminal_stop_state.request_stop([] {}, [] {});
    CHECK_FALSE(terminal_stop_state.stop_quiescent(
      terminal_state.frame_callbacks_quiescent(), true));
    stop_release.release();
    terminal_final_wake_entered.acquire();
    CHECK_FALSE(terminal_state.frame_callbacks_quiescent());
    CHECK_FALSE(terminal_stop_state.stop_quiescent(
      terminal_state.frame_callbacks_quiescent(), true));
    CHECK(terminal_quiescence_wakes == 1);
    terminal_final_wake_release.release();
    terminal_callback.join();
    CHECK(terminal_quiescence_wakes == 1);
    CHECK(terminal_stop_state.stop_quiescent(
      terminal_state.frame_callbacks_quiescent(), true));

    std::binary_semaphore stale_resize_entered{0};
    std::binary_semaphore stale_resize_release{0};
    std::uint32_t stale_resize_wakes{};
    capture_detail::FrameCallbackPolicy stale_resize_state{
      3, allocation, 7, format, [&] { ++stale_resize_wakes; }};
    stale_resize_state.open_current_epoch();
    std::jthread stale_resize_callback{[&] {
      auto admission =
        stale_resize_state.enter(stale_resize_state.current_epoch());
      stale_resize_entered.release();
      stale_resize_release.acquire();
      const auto observed = stale_resize_state.observe_frame({
        .resource_generation = 7,
        .allocation_extent = allocation,
        .content_size = {.width = 700, .height = 520},
        .surface_format = format,
      });
      static_cast<void>(observed);
    }};
    stale_resize_entered.acquire();
    stale_resize_state.request_terminal_stop();
    stale_resize_release.release();
    stale_resize_callback.join();
    const auto stale_resize_signals = stale_resize_state.drain_signals();
    CHECK_FALSE(stale_resize_signals.resize.has_value());
    CHECK(stale_resize_wakes == 1);
    CHECK(stale_resize_state.frame_callbacks_quiescent());
  }

  SECTION("signal edges and newest-only mailbox ownership")
  {
    std::uint32_t wakes{};
    capture_detail::FrameCallbackPolicy policy{3, allocation, 7, format,
                                               [&] { ++wakes; }};
    policy.open_current_epoch();
    const auto first_observation = policy.observe_frame({
      .resource_generation = 7,
      .allocation_extent = allocation,
      .content_size = {.width = 600, .height = 460},
      .surface_format = format,
    });
    CHECK(first_observation.accepted);
    CHECK(first_observation.signal_edge);
    policy.notify_mailbox_ready();

    const auto replacement_observation = policy.observe_frame({
      .resource_generation = 7,
      .allocation_extent = allocation,
      .content_size = {.width = 600, .height = 460},
      .surface_format = format,
    });
    CHECK(replacement_observation.accepted);
    CHECK_FALSE(replacement_observation.signal_edge);
    CHECK(wakes == 1);

    const auto first_signals = policy.drain_signals();
    REQUIRE(first_signals.resize.has_value());
    CHECK(*first_signals.resize == Extent{.width = 600, .height = 460});
    CHECK_FALSE(policy.drain_signals().resize.has_value());
    const auto rearmed_observation = policy.observe_frame({
      .resource_generation = 7,
      .allocation_extent = allocation,
      .content_size = {.width = 590, .height = 450},
      .surface_format = format,
    });
    CHECK(rearmed_observation.signal_edge);
    policy.notify_mailbox_ready();
    const auto rearmed_signals = policy.drain_signals();
    REQUIRE(rearmed_signals.resize.has_value());
    CHECK(*rearmed_signals.resize == Extent{.width = 590, .height = 450});

    const auto growth_observation = policy.observe_frame({
      .resource_generation = 7,
      .allocation_extent = allocation,
      .content_size = {.width = 700, .height = 520},
      .surface_format = format,
    });
    CHECK(growth_observation.accepted);
    CHECK(growth_observation.signal_edge);
    policy.notify_mailbox_ready();
    CHECK(wakes == 3);
    const auto growth_signals = policy.drain_signals();
    CHECK_FALSE(growth_signals.fatal_active);
    REQUIRE(growth_signals.resize.has_value());
    CHECK(*growth_signals.resize == Extent{.width = 700, .height = 520});
    CHECK(capture_detail::copyable_content_extent(*growth_signals.resize,
                                                  allocation) == allocation);

    FakeFrameMailbox mailbox;
    auto first_stats = std::make_shared<FakeCheckoutStats>();
    auto second_stats = std::make_shared<FakeCheckoutStats>();
    bool destroyed_outside_lock{};
    first_stats->on_destroy = [&] {
      const auto counters = mailbox.counters();
      destroyed_outside_lock =
        counters.submitted == 2 && counters.replaced == 1;
    };
    CHECK(mailbox.submit(make_fake_checkout(1, first_stats)) ==
          FakeFrameMailbox::SubmitResult::BecameReady);
    CHECK(mailbox.submit(make_fake_checkout(2, second_stats)) ==
          FakeFrameMailbox::SubmitResult::ReplacedPending);
    CHECK(first_stats->destructions == 1);
    CHECK(destroyed_outside_lock);
    auto latest = mailbox.take();
    REQUIRE(latest.has_value());
    CHECK(latest->sequence() == 2);
  }

  SECTION("owners outlive callback mailbox and render-held checkouts")
  {
    struct JobOwner final {
      std::shared_ptr<int> pool_owner;
      FakeFrameMailbox mailbox;
    };

    enum class ReleasePath { Normal,
                             Reconfigure,
                             TerminalStop };
    constexpr std::array release_paths{
      ReleasePath::Normal,
      ReleasePath::Reconfigure,
      ReleasePath::TerminalStop,
    };
    for (const auto path : release_paths) {
      CAPTURE(path);
      std::vector<int> release_order;
      bool checkout_saw_pool_alive{};
      std::weak_ptr<int> pool_lifetime;
      auto pool_owner = std::shared_ptr<int>(new int{42}, [&](int *value) {
        release_order.push_back(2);
        delete value;
      });
      pool_lifetime = pool_owner;
      auto owner = std::make_unique<JobOwner>();
      owner->pool_owner = std::move(pool_owner);
      CHECK_FALSE(pool_owner);

      auto stats = std::make_shared<FakeCheckoutStats>();
      stats->on_destroy = [&] {
        checkout_saw_pool_alive = !pool_lifetime.expired();
        release_order.push_back(1);
      };
      auto checkout = make_fake_checkout(1, stats, allocation, allocation,
                                         format, 7, pool_lifetime);
      CHECK(checkout.pool_owner_alive());
      CHECK(owner->mailbox.submit(std::move(checkout)) ==
            FakeFrameMailbox::SubmitResult::BecameReady);
      CHECK_FALSE(pool_lifetime.expired());
      CHECK(stats->moves >= 1);

      std::optional<FakeCheckout> render_held;
      if (path == ReleasePath::TerminalStop) {
        render_held = owner->mailbox.take();
        REQUIRE(render_held.has_value());
        CHECK(render_held->pool_owner_alive());
        owner->mailbox.close();
        CHECK(render_held->pool_owner_alive());
        CHECK_FALSE(pool_lifetime.expired());
        render_held.reset();
      }
      else {
        render_held = owner->mailbox.take();
        REQUIRE(render_held.has_value());
        CHECK(render_held->pool_owner_alive());
        render_held.reset();
      }

      CHECK(checkout_saw_pool_alive);
      CHECK_FALSE(pool_lifetime.expired());
      owner.reset();
      CHECK(pool_lifetime.expired());
      CHECK(release_order == std::vector{1, 2});
      CHECK(stats->destructions == 1);
    }

    auto close_count = std::make_shared<std::uint32_t>();
    {
      CHECK_THROWS_AS(
        WindowsCapturedFrame(
          1, 2, {.width = 1, .height = 1}, {.width = 1, .height = 1},
          format, std::chrono::steady_clock::time_point{},
          winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame{
            nullptr}),
        winrt::hresult_invalid_argument);
      WindowsCapturedFrame source{1,
                                  2,
                                  {.width = 1920, .height = 1080},
                                  {.width = 1280, .height = 720},
                                  format,
                                  std::chrono::steady_clock::time_point{},
                                  make_controlled_capture_frame(close_count)};
      WindowsCapturedFrame destination{std::move(source)};
      CHECK_THROWS_AS(source.texture(), winrt::hresult_illegal_method_call);
      CHECK(*close_count == 0);
      CHECK(destination.sequence() == 1);
      CHECK(destination.resource_generation() == 2);
    }
    CHECK(*close_count == 1);
  }

  SECTION("concurrent intake defers the newer frame until publication completes")
  {
    capture_detail::FrameIntakeGate gate;
    FakeFrameMailbox mailbox;
    auto first_stats = std::make_shared<FakeCheckoutStats>();
    auto second_stats = std::make_shared<FakeCheckoutStats>();
    std::atomic_uint32_t wakes{};
    capture_detail::FrameCallbackPolicy policy{3, allocation, 7, format,
                                               [&] { ++wakes; }};
    policy.open_current_epoch();
    std::binary_semaphore first_pulled{0};
    std::binary_semaphore finish_first{0};
    std::binary_semaphore contender_finished{0};
    bool first_claimed{};
    bool contender_claimed{};

    const auto release_intake =
      [&](capture_detail::FrameCallbackPolicy::Admission &admission) noexcept {
        if (gate.release()) {
          admission.record_pending();
          admission.wake_if_open();
        }
      };

    std::jthread first_intake{[&] {
      auto admission = policy.enter(policy.current_epoch());
      first_claimed = admission && gate.try_claim();
      // Pause after the native pull, before metadata and mailbox publication.
      auto frame = make_fake_checkout(1, first_stats);
      first_pulled.release();
      finish_first.acquire();
      if (first_claimed) {
        static_cast<void>(mailbox.submit(std::move(frame)));
        release_intake(*admission);
      }
    }};

    const auto first_reached_pull =
      first_pulled.try_acquire_for(std::chrono::seconds{2});
    if (!first_reached_pull) {
      finish_first.release();
      first_intake.join();
      REQUIRE(first_reached_pull);
    }

    std::jthread contender{[&] {
      auto admission = policy.enter(policy.current_epoch());
      contender_claimed = admission && gate.try_claim();
      if (contender_claimed) {
        static_cast<void>(mailbox.submit(make_fake_checkout(2, second_stats)));
        release_intake(*admission);
      }
      contender_finished.release();
    }};

    const auto contender_returned_without_waiting =
      contender_finished.try_acquire_for(std::chrono::seconds{2});
    const auto submissions_before_first_finished = mailbox.counters().submitted;
    finish_first.release();
    first_intake.join();
    contender.join();
    CHECK(first_claimed);
    CHECK(contender_returned_without_waiting);
    CHECK_FALSE(contender_claimed);
    CHECK(submissions_before_first_finished == 0);
    CHECK(policy.frame_callbacks_quiescent());
    CHECK(policy.arrival_pending());
    CHECK(wakes == 1);

    // Main-loop service consumes the rearmed arrival once the first intake exits.
    if (policy.claim_pending_arrival()) {
      auto admission = policy.enter(policy.current_epoch());
      REQUIRE(admission.has_value());
      REQUIRE(gate.try_claim());
      static_cast<void>(mailbox.submit(make_fake_checkout(2, second_stats)));
      CHECK_FALSE(gate.release());
    }
    auto latest = mailbox.take();
    REQUIRE(latest.has_value());
    CHECK(latest->sequence() == 2);
    CHECK_FALSE(policy.claim_pending_arrival());
    CHECK(policy.frame_callbacks_quiescent());
  }
}
