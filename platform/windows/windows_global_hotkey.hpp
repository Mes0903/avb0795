/// @file
/// @brief Owner-thread hotkey registration, rollback, and command-only dispatch.
/// Native implementation details remain below the portable application ports.

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

  /// Emits portable commands only; dispatch never receives a runtime or session owner.
  using HotkeyCommandSink = std::function<void(app::AppCommand)>;

  namespace detail {

    inline constexpr int primary_hotkey_id{0xA11E};
    inline constexpr int probe_hotkey_id{0xA11F};

    /// RegisterHotKey modifier/key pair derived from a validated portable chord.
    struct NativeHotkey {
      std::uint32_t modifiers{};
      std::uint32_t virtual_key{};

      friend bool operator==(const NativeHotkey &, const NativeHotkey &) = default;
    };

    /// Distinguishes an unavailable chord from a platform/ownership failure.
    enum class NativeRegistrationFailureKind { Conflict,
                                               Platform };

    /// No-throw native-call result carrying only success and fixed error facts.
    struct NativeCallResult {
      bool succeeded{};
      std::optional<std::int64_t> native_detail{};
      NativeRegistrationFailureKind failure_kind{
        NativeRegistrationFailureKind::Conflict};
    };

    using RegisterHotkeyFn = std::function<NativeCallResult(int, NativeHotkey)>;
    using UnregisterHotkeyFn = std::function<NativeCallResult(int)>;

    /// Tracks each fixed registration ID, including uncertain native cleanup.
    enum class FixedHotkeyState { Free,
                                  Registered,
                                  Uncertain };

    /// Canonical primary/probe ownership and the chord confirmed usable for dispatch.
    /// Cleanup diagnostics must not be mistaken for a successfully restored binding.
    struct HotkeyRegistrationState {
      FixedHotkeyState primary{FixedHotkeyState::Free};
      FixedHotkeyState probe{FixedHotkeyState::Free};
      std::optional<core::HotkeyChord> confirmed_chord{};
      std::optional<core::AppError> primary_cleanup_diagnostic{};
      std::optional<core::AppError> probe_cleanup_diagnostic{};
    };

    /// Convert supported keys/modifiers without parsing presentation text.
    [[nodiscard]] std::expected<NativeHotkey, core::AppError>
    map_hotkey_to_native(const core::HotkeyChord &chord) noexcept;

    /// Shared message-dispatch lifetime independent of wrapper or transaction unwind.
    /// All mutation belongs to the creating thread captured at construction.
    class HotkeyDispatchState final {
    public:
      explicit HotkeyDispatchState(HotkeyRegistrationState initial_state,
                                   HotkeyCommandSink command_sink = {});

      /// Read the current confirmed binding and native registration ownership.
      [[nodiscard]] const HotkeyRegistrationState &
      registration_state() const noexcept;
      /// Native thread owning both registration and message dispatch.
      [[nodiscard]] DWORD owner_thread_id() const noexcept;
      /// Check before making Win32 calls whose registration state is thread-local.
      [[nodiscard]] bool is_owner_thread() const noexcept;
      /// Accept only the confirmed primary chord and emit RestoreActiveReplicaEditable.
      /// Contain a throwing command sink at the Win32 callback boundary.
      void dispatch_hotkey(int id, LPARAM message_parameter) noexcept;

    private:
      friend class HotkeyRegistrationTransaction;
      [[nodiscard]] HotkeyRegistrationState &mutable_registration_state() noexcept;

      HotkeyRegistrationState registration_state_;
      HotkeyCommandSink command_sink_;
      DWORD owner_thread_id_{};
    };

    /// Replaces a chord using fixed primary/probe IDs and verified rollback state.
    /// The old confirmed chord survives a normal conflict; uncertainty remains explicit.
    class HotkeyRegistrationTransaction final {
    public:
      HotkeyRegistrationTransaction(HotkeyRegistrationState initial_state,
                                    RegisterHotkeyFn register_hotkey,
                                    UnregisterHotkeyFn unregister_hotkey);
      HotkeyRegistrationTransaction(
        std::shared_ptr<HotkeyDispatchState> dispatch_state,
        RegisterHotkeyFn register_hotkey,
        UnregisterHotkeyFn unregister_hotkey);

      /// Probe the requested chord before retiring/replacing the confirmed primary.
      [[nodiscard]] std::expected<void, core::AppError>
      replace(const core::HotkeyChord &chord) noexcept;
      /// Attempt cleanup of both fixed IDs; retained uncertainty is not hidden.
      void clear() noexcept;

      /// Expose one canonical state shared with message dispatch.
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

    /// Win32 trampoline that keeps dispatch state alive across command-sink reentrancy.
    LRESULT CALLBACK windows_global_hotkey_window_proc(
      HWND window, UINT message, WPARAM word_parameter, LPARAM long_parameter) noexcept;

    /// Move-only owner of the hidden hotkey window, class, and callback context.
    /// Native destruction must occur on its recorded owner thread.
    class HotkeyMessageWindow final {
    public:
      /// Allocate stable dispatch context before exposing it through window messages.
      [[nodiscard]] static std::expected<HotkeyMessageWindow, core::AppError>
      create(std::shared_ptr<HotkeyDispatchState> dispatch_state);

      ~HotkeyMessageWindow();

      HotkeyMessageWindow(const HotkeyMessageWindow &) = delete;
      HotkeyMessageWindow &operator=(const HotkeyMessageWindow &) = delete;
      HotkeyMessageWindow(HotkeyMessageWindow &&other) noexcept;
      HotkeyMessageWindow &operator=(HotkeyMessageWindow &&other) noexcept;

      /// Borrow the message target for registration calls while this owner lives.
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
  /// Process service adapting the portable hotkey port to one owner-thread window.
  /// A global shortcut only enqueues a command; the orchestrator resolves the active session.
  class WindowsGlobalHotkey final : public app::IGlobalHotkey {
  private:
    struct Impl;
    struct ConstructionToken {
    private:
      ConstructionToken() = default;
      friend class WindowsGlobalHotkey;
    };

  public:
    /// Build a message target and transaction state with the supplied command sink.
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

    /// Replace the native binding on the owner thread with rollback-aware results.
    [[nodiscard]] std::expected<void, core::AppError>
    replace(const core::HotkeyChord &chord) override;
    /// Best-effort owner-thread unregister; off-owner calls leave native state intact.
    void clear() noexcept override;

  private:
    std::unique_ptr<Impl> implementation_;
  };

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_GLOBAL_HOTKEY_HPP
