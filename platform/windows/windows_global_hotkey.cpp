/// @file
/// @brief Owner-thread hotkey registration, rollback, and command-only dispatch.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/windows_global_hotkey.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace airelinna::platform::windows {
  namespace detail {
    namespace {

      [[nodiscard]] constexpr core::AppError
      app_error(core::AppErrorCode code,
                std::optional<std::int64_t> native_detail = std::nullopt) noexcept
      {
        return {.code = code, .native_detail = native_detail};
      }

      [[nodiscard]] core::AppError platform_failure() noexcept
      {
        return app_error(core::AppErrorCode::PlatformOperationFailure,
                         static_cast<std::int64_t>(GetLastError()));
      }

      [[nodiscard]] constexpr std::expected<std::uint32_t, core::AppError>
      map_virtual_key(core::HotkeyKey key) noexcept
      {
        switch (key) {
        case core::HotkeyKey::A:
          return 0x41U;
        case core::HotkeyKey::B:
          return 0x42U;
        case core::HotkeyKey::C:
          return 0x43U;
        case core::HotkeyKey::D:
          return 0x44U;
        case core::HotkeyKey::E:
          return 0x45U;
        case core::HotkeyKey::F:
          return 0x46U;
        case core::HotkeyKey::G:
          return 0x47U;
        case core::HotkeyKey::H:
          return 0x48U;
        case core::HotkeyKey::I:
          return 0x49U;
        case core::HotkeyKey::J:
          return 0x4AU;
        case core::HotkeyKey::K:
          return 0x4BU;
        case core::HotkeyKey::L:
          return 0x4CU;
        case core::HotkeyKey::M:
          return 0x4DU;
        case core::HotkeyKey::N:
          return 0x4EU;
        case core::HotkeyKey::O:
          return 0x4FU;
        case core::HotkeyKey::P:
          return 0x50U;
        case core::HotkeyKey::Q:
          return 0x51U;
        case core::HotkeyKey::R:
          return 0x52U;
        case core::HotkeyKey::S:
          return 0x53U;
        case core::HotkeyKey::T:
          return 0x54U;
        case core::HotkeyKey::U:
          return 0x55U;
        case core::HotkeyKey::V:
          return 0x56U;
        case core::HotkeyKey::W:
          return 0x57U;
        case core::HotkeyKey::X:
          return 0x58U;
        case core::HotkeyKey::Y:
          return 0x59U;
        case core::HotkeyKey::Z:
          return 0x5AU;
        case core::HotkeyKey::Digit0:
          return 0x30U;
        case core::HotkeyKey::Digit1:
          return 0x31U;
        case core::HotkeyKey::Digit2:
          return 0x32U;
        case core::HotkeyKey::Digit3:
          return 0x33U;
        case core::HotkeyKey::Digit4:
          return 0x34U;
        case core::HotkeyKey::Digit5:
          return 0x35U;
        case core::HotkeyKey::Digit6:
          return 0x36U;
        case core::HotkeyKey::Digit7:
          return 0x37U;
        case core::HotkeyKey::Digit8:
          return 0x38U;
        case core::HotkeyKey::Digit9:
          return 0x39U;
        case core::HotkeyKey::F1:
          return VK_F1;
        case core::HotkeyKey::F2:
          return VK_F2;
        case core::HotkeyKey::F3:
          return VK_F3;
        case core::HotkeyKey::F4:
          return VK_F4;
        case core::HotkeyKey::F5:
          return VK_F5;
        case core::HotkeyKey::F6:
          return VK_F6;
        case core::HotkeyKey::F7:
          return VK_F7;
        case core::HotkeyKey::F8:
          return VK_F8;
        case core::HotkeyKey::F9:
          return VK_F9;
        case core::HotkeyKey::F10:
          return VK_F10;
        case core::HotkeyKey::F11:
          return VK_F11;
        case core::HotkeyKey::F12:
          return VK_F12;
        }

        return std::unexpected{app_error(core::AppErrorCode::InvalidValue)};
      }

      [[nodiscard]] core::AppError
      call_error(core::AppErrorCode code, const NativeCallResult &result) noexcept
      {
        return app_error(code, result.native_detail);
      }

      [[nodiscard]] core::AppError
      registration_error(const NativeCallResult &result) noexcept
      {
        const auto code =
          result.failure_kind == NativeRegistrationFailureKind::Conflict
            ? core::AppErrorCode::HotkeyConflict
            : core::AppErrorCode::PlatformOperationFailure;
        return call_error(code, result);
      }

      [[nodiscard]] constexpr core::AppError thread_affinity_error() noexcept
      {
        return app_error(core::AppErrorCode::PlatformOperationFailure,
                         ERROR_INVALID_THREAD_ID);
      }

      void record_cleanup_result(FixedHotkeyState previous,
                                 FixedHotkeyState &registration,
                                 std::optional<core::AppError> &diagnostic,
                                 const NativeCallResult &result) noexcept
      {
        if (result.succeeded) {
          registration = FixedHotkeyState::Free;
          diagnostic.reset();
          return;
        }

        if (previous != FixedHotkeyState::Free) {
          registration = FixedHotkeyState::Uncertain;
        }
        diagnostic = call_error(core::AppErrorCode::HotkeyCleanupFailure, result);
      }

    }    // namespace

    std::expected<NativeHotkey, core::AppError>
    map_hotkey_to_native(const core::HotkeyChord &chord) noexcept
    {
      const auto virtual_key = map_virtual_key(chord.key);
      if (!virtual_key) {
        return std::unexpected{virtual_key.error()};
      }

      std::uint32_t modifiers{};
      if (chord.modifiers.control) {
        modifiers |= MOD_CONTROL;
      }
      if (chord.modifiers.alt) {
        modifiers |= MOD_ALT;
      }
      if (chord.modifiers.shift) {
        modifiers |= MOD_SHIFT;
      }
      if (chord.modifiers.meta) {
        modifiers |= MOD_WIN;
      }
      return NativeHotkey{.modifiers = modifiers, .virtual_key = *virtual_key};
    }

    HotkeyDispatchState::HotkeyDispatchState(HotkeyRegistrationState initial_state,
                                             HotkeyCommandSink command_sink)
        : registration_state_{std::move(initial_state)},
          command_sink_{std::move(command_sink)},
          owner_thread_id_{GetCurrentThreadId()} {}

    const HotkeyRegistrationState &
    HotkeyDispatchState::registration_state() const noexcept
    {
      return registration_state_;
    }

    DWORD HotkeyDispatchState::owner_thread_id() const noexcept
    {
      return owner_thread_id_;
    }

    bool HotkeyDispatchState::is_owner_thread() const noexcept
    {
      return GetCurrentThreadId() == owner_thread_id_;
    }

    void HotkeyDispatchState::dispatch_hotkey(int id,
                                              LPARAM message_parameter) noexcept
    {
      if (id != primary_hotkey_id ||
          registration_state_.primary != FixedHotkeyState::Registered ||
          !registration_state_.confirmed_chord || !command_sink_) {
        return;
      }

      // Ignore stale WM_HOTKEY payloads left from a replaced binding.
      const auto confirmed_native =
        map_hotkey_to_native(*registration_state_.confirmed_chord);
      if (!confirmed_native ||
          LOWORD(message_parameter) != confirmed_native->modifiers ||
          HIWORD(message_parameter) != confirmed_native->virtual_key) {
        return;
      }

      try {
        command_sink_(app::AppCommand{app::RestoreActiveReplicaEditable{}});
      } catch (...) {
        // A callback failure drops only this restore request. The controller
        // remains the independent recovery path.
      }
    }

    HotkeyRegistrationState &
    HotkeyDispatchState::mutable_registration_state() noexcept
    {
      return registration_state_;
    }

    HotkeyRegistrationTransaction::HotkeyRegistrationTransaction(
      HotkeyRegistrationState initial_state, RegisterHotkeyFn register_hotkey, UnregisterHotkeyFn unregister_hotkey)
        : HotkeyRegistrationTransaction{
            std::make_shared<HotkeyDispatchState>(std::move(initial_state)),
            std::move(register_hotkey), std::move(unregister_hotkey)} {}

    HotkeyRegistrationTransaction::HotkeyRegistrationTransaction(
      std::shared_ptr<HotkeyDispatchState> dispatch_state,
      RegisterHotkeyFn register_hotkey,
      UnregisterHotkeyFn unregister_hotkey)
        : dispatch_state_{std::move(dispatch_state)},
          register_hotkey_{std::move(register_hotkey)},
          unregister_hotkey_{std::move(unregister_hotkey)}
    {
      if (!dispatch_state_) {
        throw std::invalid_argument{"hotkey dispatch state must not be null"};
      }
      if (!register_hotkey_ || !unregister_hotkey_) {
        throw std::invalid_argument{"hotkey registration callables must be set"};
      }
    }

    std::expected<void, core::AppError> HotkeyRegistrationTransaction::replace(
      const core::HotkeyChord &chord) noexcept
    {
      if (!dispatch_state_->is_owner_thread()) {
        return std::unexpected{thread_affinity_error()};
      }
      auto &state = dispatch_state_->mutable_registration_state();
      const auto native_hotkey = map_hotkey_to_native(chord);
      if (!native_hotkey) {
        return std::unexpected{native_hotkey.error()};
      }

      const auto cleanup_pending = state.primary == FixedHotkeyState::Uncertain ||
                                   state.probe == FixedHotkeyState::Uncertain;
      if (!cleanup_pending && state.confirmed_chord == chord) {
        return {};
      }

      retry_uncertain_cleanup();
      if (state.primary == FixedHotkeyState::Uncertain ||
          state.probe == FixedHotkeyState::Uncertain) {
        const auto &diagnostic = state.primary == FixedHotkeyState::Uncertain
                                   ? state.primary_cleanup_diagnostic
                                   : state.probe_cleanup_diagnostic;
        return std::unexpected{diagnostic.value_or(
          app_error(core::AppErrorCode::HotkeyCleanupFailure))};
      }

      // Test availability before unregistering a usable old primary chord.
      const auto probe_registration =
        register_hotkey(probe_hotkey_id, *native_hotkey);
      if (!probe_registration.succeeded) {
        return std::unexpected{registration_error(probe_registration)};
      }
      state.probe = FixedHotkeyState::Registered;
      state.probe_cleanup_diagnostic.reset();

      const auto probe_cleanup = unregister_hotkey(probe_hotkey_id);
      if (!probe_cleanup.succeeded) {
        state.probe = FixedHotkeyState::Uncertain;
        state.probe_cleanup_diagnostic =
          call_error(core::AppErrorCode::HotkeyCleanupFailure, probe_cleanup);
        return std::unexpected{*state.probe_cleanup_diagnostic};
      }
      state.probe = FixedHotkeyState::Free;
      state.probe_cleanup_diagnostic.reset();

      // Preserve rollback input separately from the dispatch-visible confirmed binding.
      const auto previous_chord = state.confirmed_chord;
      if (previous_chord) {
        const auto primary_cleanup = unregister_hotkey(primary_hotkey_id);
        if (!primary_cleanup.succeeded) {
          state.primary = FixedHotkeyState::Uncertain;
          state.confirmed_chord.reset();
          state.primary_cleanup_diagnostic =
            call_error(core::AppErrorCode::HotkeyCleanupFailure, primary_cleanup);
          return std::unexpected{call_error(
            core::AppErrorCode::HotkeyRegistrationLost, primary_cleanup)};
        }
        state.primary = FixedHotkeyState::Free;
        state.primary_cleanup_diagnostic.reset();
        state.confirmed_chord.reset();
      }

      const auto primary_registration =
        register_hotkey(primary_hotkey_id, *native_hotkey);
      if (primary_registration.succeeded) {
        state.primary = FixedHotkeyState::Registered;
        state.primary_cleanup_diagnostic.reset();
        state.confirmed_chord = chord;
        return {};
      }

      state.primary = FixedHotkeyState::Free;
      if (!previous_chord) {
        return std::unexpected{registration_error(primary_registration)};
      }

      const auto previous_native_hotkey = map_hotkey_to_native(*previous_chord);
      if (!previous_native_hotkey) {
        state.confirmed_chord.reset();
        return std::unexpected{
          app_error(core::AppErrorCode::HotkeyRegistrationLost)};
      }
      // A failed replacement is ordinary only if the previous registration is restored.
      const auto rollback =
        register_hotkey(primary_hotkey_id, *previous_native_hotkey);
      if (rollback.succeeded) {
        state.primary = FixedHotkeyState::Registered;
        state.primary_cleanup_diagnostic.reset();
        state.confirmed_chord = previous_chord;
        return std::unexpected{registration_error(primary_registration)};
      }

      state.confirmed_chord.reset();
      return std::unexpected{
        call_error(core::AppErrorCode::HotkeyRegistrationLost, rollback)};
    }

    void HotkeyRegistrationTransaction::clear() noexcept
    {
      if (!dispatch_state_->is_owner_thread()) {
        return;
      }
      auto &state = dispatch_state_->mutable_registration_state();
      // Disable dispatch before cleanup; uncertain native registrations must remain inert.
      state.confirmed_chord.reset();

      const auto previous_primary = state.primary;
      const auto primary_cleanup = unregister_hotkey(primary_hotkey_id);
      record_cleanup_result(previous_primary, state.primary,
                            state.primary_cleanup_diagnostic, primary_cleanup);

      const auto previous_probe = state.probe;
      const auto probe_cleanup = unregister_hotkey(probe_hotkey_id);
      record_cleanup_result(previous_probe, state.probe,
                            state.probe_cleanup_diagnostic, probe_cleanup);
    }

    const HotkeyRegistrationState &
    HotkeyRegistrationTransaction::state() const noexcept
    {
      return dispatch_state_->registration_state();
    }

    NativeCallResult
    HotkeyRegistrationTransaction::register_hotkey(int id,
                                                   NativeHotkey hotkey) noexcept
    {
      try {
        return register_hotkey_(id, hotkey);
      } catch (...) {
        return {
          .succeeded = false,
          .failure_kind = NativeRegistrationFailureKind::Platform,
        };
      }
    }

    NativeCallResult
    HotkeyRegistrationTransaction::unregister_hotkey(int id) noexcept
    {
      try {
        return unregister_hotkey_(id);
      } catch (...) {
        return {
          .succeeded = false,
          .failure_kind = NativeRegistrationFailureKind::Platform,
        };
      }
    }

    void HotkeyRegistrationTransaction::retry_uncertain_cleanup() noexcept
    {
      auto &state = dispatch_state_->mutable_registration_state();
      if (state.primary == FixedHotkeyState::Uncertain) {
        const auto cleanup = unregister_hotkey(primary_hotkey_id);
        record_cleanup_result(FixedHotkeyState::Uncertain, state.primary,
                              state.primary_cleanup_diagnostic, cleanup);
      }
      if (state.probe == FixedHotkeyState::Uncertain) {
        const auto cleanup = unregister_hotkey(probe_hotkey_id);
        record_cleanup_result(FixedHotkeyState::Uncertain, state.probe,
                              state.probe_cleanup_diagnostic, cleanup);
      }
    }

    // Window user data owns shared dispatch lifetime, not a raw pointer to the service wrapper.
    struct HotkeyWindowContext {
      std::shared_ptr<HotkeyDispatchState> dispatch_state;
    };

    LRESULT CALLBACK windows_global_hotkey_window_proc(
      HWND window, UINT message, WPARAM word_parameter, LPARAM long_parameter) noexcept
    {
      try {
        if (message == WM_NCCREATE) {
          const auto *const creation =
            reinterpret_cast<const CREATESTRUCTW *>(long_parameter);
          auto *const context =
            static_cast<HotkeyWindowContext *>(creation->lpCreateParams);
          SetLastError(ERROR_SUCCESS);
          const auto previous = SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
          if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
            return FALSE;
          }
          return TRUE;
        }

        auto *const context = reinterpret_cast<HotkeyWindowContext *>(
          GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_HOTKEY) {
          if (context != nullptr &&
              word_parameter == static_cast<WPARAM>(primary_hotkey_id)) {
            // A command sink may destroy the wrapper/window; keep dispatch alive until return.
            const auto dispatch_state = context->dispatch_state;
            dispatch_state->dispatch_hotkey(primary_hotkey_id, long_parameter);
          }
          return 0;
        }
        if (message == WM_NCDESTROY) {
          SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(window, message, word_parameter, long_parameter);
      } catch (...) {
        return 0;
      }
    }

    std::expected<HotkeyMessageWindow, core::AppError> HotkeyMessageWindow::create(
      std::shared_ptr<HotkeyDispatchState> dispatch_state)
    {
      if (!dispatch_state) {
        return std::unexpected{
          app_error(core::AppErrorCode::InvalidValue, ERROR_INVALID_HANDLE)};
      }
      if (!dispatch_state->is_owner_thread()) {
        return std::unexpected{thread_affinity_error()};
      }
      // Construct callback-visible ownership before CreateWindowEx sends WM_NCCREATE.
      auto context = std::make_unique<HotkeyWindowContext>(HotkeyWindowContext{
        .dispatch_state = std::move(dispatch_state),
      });
      const auto instance = GetModuleHandleW(nullptr);
      if (instance == nullptr) {
        return std::unexpected{platform_failure()};
      }

      auto class_name =
        std::wstring{L"Airelinna.WindowsGlobalHotkey."} +
        std::to_wstring(GetCurrentProcessId()) + L'.' +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(context.get()));
      WNDCLASSEXW window_class{};
      window_class.cbSize = sizeof(window_class);
      window_class.lpfnWndProc = windows_global_hotkey_window_proc;
      window_class.hInstance = instance;
      window_class.lpszClassName = class_name.c_str();
      if (RegisterClassExW(&window_class) == 0) {
        return std::unexpected{platform_failure()};
      }

      const auto window =
        CreateWindowExW(0, class_name.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                        nullptr, instance, context.get());
      if (window == nullptr) {
        const auto failure = platform_failure();
        UnregisterClassW(class_name.c_str(), instance);
        return std::unexpected{failure};
      }

      const auto owner_thread_id = context->dispatch_state->owner_thread_id();
      return HotkeyMessageWindow{std::move(context), window, instance,
                                 std::move(class_name), owner_thread_id};
    }

    HotkeyMessageWindow::HotkeyMessageWindow(
      std::unique_ptr<HotkeyWindowContext> context, HWND window, HINSTANCE instance, std::wstring class_name, DWORD owner_thread_id) noexcept
        : context_{std::move(context)}, window_{window}, instance_{instance}, class_name_{std::move(class_name)}, owner_thread_id_{owner_thread_id} {}

    HotkeyMessageWindow::~HotkeyMessageWindow() { destroy(); }

    HotkeyMessageWindow::HotkeyMessageWindow(HotkeyMessageWindow &&other) noexcept
        : context_{std::move(other.context_)},
          window_{std::exchange(other.window_, nullptr)},
          instance_{std::exchange(other.instance_, nullptr)},
          class_name_{std::move(other.class_name_)},
          owner_thread_id_{std::exchange(other.owner_thread_id_, 0)} {}

    HotkeyMessageWindow &
    HotkeyMessageWindow::operator=(HotkeyMessageWindow &&other) noexcept
    {
      if (this != &other) {
        destroy();
        context_ = std::move(other.context_);
        window_ = std::exchange(other.window_, nullptr);
        instance_ = std::exchange(other.instance_, nullptr);
        class_name_ = std::move(other.class_name_);
        owner_thread_id_ = std::exchange(other.owner_thread_id_, 0);
      }
      return *this;
    }

    HWND HotkeyMessageWindow::native_handle() const noexcept { return window_; }

    void HotkeyMessageWindow::destroy() noexcept
    {
      if (window_ != nullptr && GetCurrentThreadId() != owner_thread_id_) {
        std::terminate();
      }

      if (window_ != nullptr) {
        SetLastError(ERROR_SUCCESS);
        const auto previous = SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
          std::terminate();
        }
        // GWLP_USERDATA was cleared successfully before callback-owned state is released.
        context_.reset();

        if (DestroyWindow(window_) == FALSE) {
          // The native window remains process-owned but is inert: its user data
          // was detached before the owned callback state was released.
          window_ = nullptr;
          instance_ = nullptr;
          class_name_.clear();
          owner_thread_id_ = 0;
          return;
        }
      }
      window_ = nullptr;
      if (instance_ != nullptr && !class_name_.empty()) {
        UnregisterClassW(class_name_.c_str(), instance_);
      }
      instance_ = nullptr;
      class_name_.clear();
      context_.reset();
      owner_thread_id_ = 0;
    }

  }    // namespace detail

  struct WindowsGlobalHotkey::Impl final {
    explicit Impl(HotkeyCommandSink sink)
        : dispatch_state{std::make_shared<detail::HotkeyDispatchState>(
            detail::HotkeyRegistrationState{}, std::move(sink))},
          transaction{
            dispatch_state,
            [this](int id, detail::NativeHotkey hotkey) noexcept {
              if (!window) {
                return detail::NativeCallResult{
                  .succeeded = false,
                  .failure_kind =
                    detail::NativeRegistrationFailureKind::Platform,
                };
              }
              if (RegisterHotKey(window->native_handle(), id,
                                 static_cast<UINT>(hotkey.modifiers),
                                 static_cast<UINT>(hotkey.virtual_key)) ==
                  FALSE) {
                const auto native_detail = GetLastError();
                return detail::NativeCallResult{
                  .succeeded = false,
                  .native_detail = static_cast<std::int64_t>(native_detail),
                  .failure_kind =
                    native_detail == ERROR_HOTKEY_ALREADY_REGISTERED
                      ? detail::NativeRegistrationFailureKind::Conflict
                      : detail::NativeRegistrationFailureKind::Platform,
                };
              }
              return detail::NativeCallResult{.succeeded = true};
            },
            [this](int id) noexcept {
              if (!window) {
                return detail::NativeCallResult{.succeeded = false};
              }
              if (UnregisterHotKey(window->native_handle(), id) == FALSE) {
                return detail::NativeCallResult{
                  .succeeded = false,
                  .native_detail = static_cast<std::int64_t>(GetLastError()),
                };
              }
              return detail::NativeCallResult{.succeeded = true};
            }} {}

    ~Impl()
    {
      if (!dispatch_state->is_owner_thread()) {
        std::terminate();
      }
      // Unregister while the message target still exists, then detach/destroy that target.
      clear();
      window.reset();
    }

    [[nodiscard]] static std::expected<std::unique_ptr<Impl>, core::AppError>
    create(HotkeyCommandSink sink)
    {
      auto implementation = std::make_unique<Impl>(std::move(sink));
      auto message_window =
        detail::HotkeyMessageWindow::create(implementation->dispatch_state);
      if (!message_window) {
        return std::unexpected{message_window.error()};
      }
      implementation->window.emplace(std::move(*message_window));
      return implementation;
    }

    [[nodiscard]] std::expected<void, core::AppError>
    replace(const core::HotkeyChord &chord) noexcept
    {
      if (!dispatch_state->is_owner_thread()) {
        return std::unexpected{core::AppError{
          .code = core::AppErrorCode::PlatformOperationFailure,
          .native_detail = ERROR_INVALID_THREAD_ID,
        }};
      }
      clear_performed = false;
      return transaction.replace(chord);
    }

    void clear() noexcept
    {
      if (!dispatch_state->is_owner_thread() || !window || clear_performed) {
        return;
      }
      transaction.clear();
      clear_performed = true;
    }

    std::shared_ptr<detail::HotkeyDispatchState> dispatch_state;
    detail::HotkeyRegistrationTransaction transaction;
    std::optional<detail::HotkeyMessageWindow> window;
    bool clear_performed{};
  };

  std::expected<std::unique_ptr<WindowsGlobalHotkey>, core::AppError>
  WindowsGlobalHotkey::create(HotkeyCommandSink command_sink)
  {
    if (!command_sink) {
      return std::unexpected{
        core::AppError{.code = core::AppErrorCode::InvalidValue}};
    }
    auto implementation = Impl::create(std::move(command_sink));
    if (!implementation) {
      return std::unexpected{implementation.error()};
    }
    return std::make_unique<WindowsGlobalHotkey>(ConstructionToken{},
                                                 std::move(*implementation));
  }

  WindowsGlobalHotkey::WindowsGlobalHotkey(
    ConstructionToken, std::unique_ptr<Impl> implementation) noexcept
      : implementation_{std::move(implementation)} {}

  WindowsGlobalHotkey::~WindowsGlobalHotkey() = default;

  std::expected<void, core::AppError>
  WindowsGlobalHotkey::replace(const core::HotkeyChord &chord)
  {
    return implementation_->replace(chord);
  }

  void WindowsGlobalHotkey::clear() noexcept { implementation_->clear(); }

}    // namespace airelinna::platform::windows
