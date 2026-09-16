/// @file
/// @brief Owner-thread hotkey registration, rollback, and typed chord dispatch.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_GLOBAL_HOTKEY_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_GLOBAL_HOTKEY_HPP

#include "avb/app/ports.hpp"
#include "avb/core/types.hpp"

#include <windows.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace avb::platform::windows {

  /// @brief Deliver a confirmed chord value for composition-owned event routing.
  /// The sink selects the recorder or application recovery path while native
  /// registration and payload validation remain owned by this service.
  using HotkeyChordSink = std::function<void(core::HotkeyChord)>;

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

    /// @brief Retain confirmed dispatch state across chord-sink reentrancy.
    ///
    /// The native trampoline keeps a shared owner during sink invocation, so the
    /// sink may destroy the service without invalidating the executing dispatch.
    /// Shared ownership supplies lifetime only: state mutation and dispatch belong
    /// to the creating thread and are not synchronized for concurrent access.
    class HotkeyDispatchState final {
    public:
      explicit HotkeyDispatchState(HotkeyRegistrationState initial_state,
                                   HotkeyChordSink chord_sink = {});

      /// Read the current confirmed binding and native registration ownership.
      [[nodiscard]] const HotkeyRegistrationState &
      registration_state() const noexcept;
      /// Native thread owning both registration and message dispatch.
      [[nodiscard]] DWORD owner_thread_id() const noexcept;
      /// Check before making Win32 calls whose registration state is thread-local.
      [[nodiscard]] bool is_owner_thread() const noexcept;
      /// @brief Validate the primary ID and payload, then deliver its confirmed chord.
      /// The copied value survives reentrant registration changes. Exceptions
      /// from the consumer are contained at this Win32 callback boundary.
      void dispatch_hotkey(int id, LPARAM message_parameter) noexcept;

    private:
      friend class HotkeyRegistrationTransaction;
      [[nodiscard]] HotkeyRegistrationState &mutable_registration_state() noexcept;

      HotkeyRegistrationState registration_state_;
      HotkeyChordSink chord_sink_;
      DWORD owner_thread_id_{};
    };

    /// @brief Replace a chord while tracking primary and probe ownership separately.
    ///
    /// Probing protects the old registration from an ordinary availability
    /// conflict, but cannot reserve the chord across the later replacement call.
    /// Failure after retiring the old primary therefore attempts restoration.
    /// Only a confirmed primary may dispatch; uncertain native ownership retains
    /// cleanup diagnostics and blocks replacement until cleanup succeeds.
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

    /// Win32 trampoline that keeps dispatch state alive across chord-sink reentrancy.
    LRESULT CALLBACK windows_global_hotkey_window_proc(
      HWND window, UINT message, WPARAM word_parameter, LPARAM long_parameter) noexcept;

    /// @brief Own the hidden message target, registered class, and dispatch context.
    ///
    /// Moving transfers native cleanup responsibility without changing the owner
    /// thread. Destruction first detaches user data, then releases context and
    /// destroys the window. If destruction fails, the remaining native window is
    /// inert; wrong-thread destruction or failed context detachment terminates.
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

  /// @brief Adapt the global shortcut port to an owner-thread message window.
  ///
  /// A shortcut delivers its confirmed chord; the composition-supplied sink
  /// chooses recorder input or application recovery. The creating thread owns
  /// registration, dispatch, and destruction.
  /// Off-owner replace reports a platform error and clear does nothing; destroying
  /// on another thread terminates. A copied chord sink does not extend lifetimes
  /// of objects it captures by reference, which must survive message dispatch.
  class WindowsGlobalHotkey final : public app::IGlobalHotkey {
  private:
    struct Impl;
    struct ConstructionToken {
    private:
      ConstructionToken() = default;
      friend class WindowsGlobalHotkey;
    };

  public:
    /// @brief Build an initially unregistered service around the supplied chord sink.
    ///
    /// An empty sink or native window-creation failure returns a typed error.
    /// Allocation failures may throw. The caller must pump messages on the
    /// creating thread for registered shortcuts to reach the sink.
    [[nodiscard]] static std::expected<std::unique_ptr<WindowsGlobalHotkey>,
                                       core::AppError>
    create(HotkeyChordSink chord_sink);

    WindowsGlobalHotkey(ConstructionToken,
                        std::unique_ptr<Impl> implementation) noexcept;
    ~WindowsGlobalHotkey() override;

    WindowsGlobalHotkey(const WindowsGlobalHotkey &) = delete;
    WindowsGlobalHotkey &operator=(const WindowsGlobalHotkey &) = delete;
    WindowsGlobalHotkey(WindowsGlobalHotkey &&) = delete;
    WindowsGlobalHotkey &operator=(WindowsGlobalHotkey &&) = delete;

    /// @brief Replace the binding while preserving a confirmed old chord when possible.
    ///
    /// A conflict before primary retirement leaves the old chord usable. Failure
    /// to restore it reports HotkeyRegistrationLost; uncertain unregister state
    /// remains tracked instead of being treated as a successful replacement.
    [[nodiscard]] std::expected<void, core::AppError>
    replace(const core::HotkeyChord &chord) override;
    /// Best-effort owner-thread unregister; off-owner calls leave native state intact.
    void clear() noexcept override;

  private:
    std::unique_ptr<Impl> implementation_;
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_GLOBAL_HOTKEY_HPP
