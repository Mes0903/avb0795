#ifndef AIRELINNA_PLATFORM_WINDOWS_WINDOWS_GLOBAL_HOTKEY_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WINDOWS_GLOBAL_HOTKEY_HPP

#include "airelinna/app/commands.hpp"
#include "airelinna/app/ports.hpp"
#include "airelinna/core/types.hpp"

#include <windows.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace airelinna::platform::windows {

  using HotkeyCommandSink = std::function<void(app::AppCommand)>;

  namespace detail {

    inline constexpr int primary_hotkey_id{0xA11E};
    inline constexpr int probe_hotkey_id{0xA11F};

    struct NativeHotkey {
      std::uint32_t modifiers{};
      std::uint32_t virtual_key{};

      friend bool operator==(const NativeHotkey &, const NativeHotkey &) = default;
    };

    enum class NativeRegistrationFailureKind { Conflict,
                                               Platform };

    struct NativeCallResult {
      bool succeeded{};
      std::optional<std::int64_t> native_detail{};
      NativeRegistrationFailureKind failure_kind{
        NativeRegistrationFailureKind::Conflict};
    };

    using RegisterHotkeyFn = std::function<NativeCallResult(int, NativeHotkey)>;
    using UnregisterHotkeyFn = std::function<NativeCallResult(int)>;

    enum class FixedHotkeyState { Free,
                                  Registered,
                                  Uncertain };

    struct HotkeyRegistrationState {
      FixedHotkeyState primary{FixedHotkeyState::Free};
      FixedHotkeyState probe{FixedHotkeyState::Free};
      std::optional<core::HotkeyChord> confirmed_chord{};
      std::optional<core::AppError> primary_cleanup_diagnostic{};
      std::optional<core::AppError> probe_cleanup_diagnostic{};
    };

    [[nodiscard]] std::expected<NativeHotkey, core::AppError>
    map_hotkey_to_native(const core::HotkeyChord &chord) noexcept;

    class HotkeyDispatchState final {
    public:
      explicit HotkeyDispatchState(HotkeyRegistrationState initial_state,
                                   HotkeyCommandSink command_sink = {});

      [[nodiscard]] const HotkeyRegistrationState &
      registration_state() const noexcept;
      [[nodiscard]] DWORD owner_thread_id() const noexcept;
      [[nodiscard]] bool is_owner_thread() const noexcept;
      void dispatch_hotkey(int id, LPARAM message_parameter) noexcept;

    private:
      friend class HotkeyRegistrationTransaction;
      [[nodiscard]] HotkeyRegistrationState &mutable_registration_state() noexcept;

      HotkeyRegistrationState registration_state_;
      HotkeyCommandSink command_sink_;
      DWORD owner_thread_id_{};
    };

    class HotkeyRegistrationTransaction final {
    public:
      HotkeyRegistrationTransaction(HotkeyRegistrationState initial_state,
                                    RegisterHotkeyFn register_hotkey,
                                    UnregisterHotkeyFn unregister_hotkey);
      HotkeyRegistrationTransaction(
        std::shared_ptr<HotkeyDispatchState> dispatch_state,
        RegisterHotkeyFn register_hotkey,
        UnregisterHotkeyFn unregister_hotkey);

      [[nodiscard]] std::expected<void, core::AppError>
      replace(const core::HotkeyChord &chord) noexcept;
      void clear() noexcept;

      [[nodiscard]] const HotkeyRegistrationState &state() const noexcept;

    private:
      [[nodiscard]] NativeCallResult register_hotkey(int id,
                                                     NativeHotkey hotkey) noexcept;
      [[nodiscard]] NativeCallResult unregister_hotkey(int id) noexcept;
      void retry_uncertain_cleanup() noexcept;

      std::shared_ptr<HotkeyDispatchState> dispatch_state_;
      RegisterHotkeyFn register_hotkey_;
      UnregisterHotkeyFn unregister_hotkey_;
    };

    struct HotkeyWindowContext;

    LRESULT CALLBACK windows_global_hotkey_window_proc(
      HWND window, UINT message, WPARAM word_parameter, LPARAM long_parameter) noexcept;

    class HotkeyMessageWindow final {
    public:
      [[nodiscard]] static std::expected<HotkeyMessageWindow, core::AppError>
      create(std::shared_ptr<HotkeyDispatchState> dispatch_state);

      ~HotkeyMessageWindow();

      HotkeyMessageWindow(const HotkeyMessageWindow &) = delete;
      HotkeyMessageWindow &operator=(const HotkeyMessageWindow &) = delete;
      HotkeyMessageWindow(HotkeyMessageWindow &&other) noexcept;
      HotkeyMessageWindow &operator=(HotkeyMessageWindow &&other) noexcept;

      [[nodiscard]] HWND native_handle() const noexcept;

    private:
      HotkeyMessageWindow(std::unique_ptr<HotkeyWindowContext> context, HWND window, HINSTANCE instance, std::wstring class_name, DWORD owner_thread_id) noexcept;
      void destroy() noexcept;

      std::unique_ptr<HotkeyWindowContext> context_;
      HWND window_{};
      HINSTANCE instance_{};
      std::wstring class_name_;
      DWORD owner_thread_id_{};
    };

  }    // namespace detail

  // The creating thread owns all native operations. Off-owner replace reports a
  // platform error and clear is a no-op. Destruction must occur on the creating
  // thread; violating that lifetime invariant terminates in every build mode.
  class WindowsGlobalHotkey final : public app::IGlobalHotkey {
  private:
    struct Impl;
    struct ConstructionToken {
    private:
      ConstructionToken() = default;
      friend class WindowsGlobalHotkey;
    };

  public:
    [[nodiscard]] static std::expected<std::unique_ptr<WindowsGlobalHotkey>,
                                       core::AppError>
    create(HotkeyCommandSink command_sink);

    WindowsGlobalHotkey(ConstructionToken,
                        std::unique_ptr<Impl> implementation) noexcept;
    ~WindowsGlobalHotkey() override;

    WindowsGlobalHotkey(const WindowsGlobalHotkey &) = delete;
    WindowsGlobalHotkey &operator=(const WindowsGlobalHotkey &) = delete;
    WindowsGlobalHotkey(WindowsGlobalHotkey &&) = delete;
    WindowsGlobalHotkey &operator=(WindowsGlobalHotkey &&) = delete;

    [[nodiscard]] std::expected<void, core::AppError>
    replace(const core::HotkeyChord &chord) override;
    void clear() noexcept override;

  private:
    std::unique_ptr<Impl> implementation_;
  };

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_GLOBAL_HOTKEY_HPP
