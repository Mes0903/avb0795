#include "platform/windows/win32_overlay_adapter.hpp"

#include "platform/windows/detail/overlay_callback.hpp"
#include "platform/windows/overlay_window.hpp"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace airelinna::platform::windows {
  namespace {

    constexpr wchar_t overlay_adapter_property[]{L"Airelinna.Win32OverlayThunk.v1"};
    constexpr std::uint32_t logical_resize_band{8};

    static_assert(detail::class_style_own_dc_bit == CS_OWNDC);
    static_assert(detail::class_style_class_dc_bit == CS_CLASSDC);
    static_assert(detail::transparent_style_bit == WS_EX_TRANSPARENT);
    static_assert(detail::layered_style_bit == WS_EX_LAYERED);
    static_assert(detail::layered_alpha_flag == LWA_ALPHA);

    [[nodiscard]] constexpr core::AppError adapter_error(
      const core::AppErrorCode code,
      const std::optional<std::int64_t> native_detail = std::nullopt) noexcept
    {
      return {.code = code, .native_detail = native_detail};
    }

    [[nodiscard]] core::AppError last_platform_error() noexcept
    {
      return adapter_error(core::AppErrorCode::PlatformOperationFailure,
                           static_cast<std::int64_t>(GetLastError()));
    }

    [[nodiscard]] std::expected<LONG_PTR, core::AppError>
    read_window_long(const HWND window, const int index) noexcept
    {
      SetLastError(ERROR_SUCCESS);
      const auto value = GetWindowLongPtrW(window, index);
      if (value == 0 && GetLastError() != ERROR_SUCCESS) {
        return std::unexpected{last_platform_error()};
      }
      return value;
    }

    [[nodiscard]] std::expected<LONG_PTR, core::AppError>
    write_window_long(const HWND window, const int index, const LONG_PTR value) noexcept
    {
      SetLastError(ERROR_SUCCESS);
      const auto previous = SetWindowLongPtrW(window, index, value);
      if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
        return std::unexpected{last_platform_error()};
      }
      return previous;
    }

    [[nodiscard]] detail::OverlayPropertyRemoval
    remove_and_observe_overlay_property(const HWND window,
                                        const HANDLE expected) noexcept
    {
      const auto removed = RemovePropW(window, overlay_adapter_property);
      if (GetPropW(window, overlay_adapter_property) != nullptr) {
        return detail::OverlayPropertyRemoval::StillPresent;
      }
      return removed == expected ? detail::OverlayPropertyRemoval::RemovedExpected
                                 : detail::OverlayPropertyRemoval::AbsentUnexpected;
    }

    [[nodiscard]] std::expected<detail::FrameInsets, core::AppError>
    frame_insets(const HWND window, const std::uint32_t dpi) noexcept
    {
      const auto style = read_window_long(window, GWL_STYLE);
      const auto extended_style = read_window_long(window, GWL_EXSTYLE);
      if (!style || !extended_style) {
        return std::unexpected{!style ? style.error() : extended_style.error()};
      }
      constexpr LONG probe_extent{1};
      RECT adjusted{
        .left = 0, .top = 0, .right = probe_extent, .bottom = probe_extent};
      if (AdjustWindowRectExForDpi(&adjusted, static_cast<DWORD>(*style), FALSE,
                                   static_cast<DWORD>(*extended_style),
                                   dpi) == FALSE) {
        return std::unexpected{last_platform_error()};
      }
      return detail::FrameInsets{
        .left = -adjusted.left,
        .top = -adjusted.top,
        .right = adjusted.right - probe_extent,
        .bottom = adjusted.bottom - probe_extent,
      };
    }

    class Win32StyleOperations final {
    public:
      explicit Win32StyleOperations(const HWND window) noexcept : window_{window} {}

      [[nodiscard]] std::expected<std::uintptr_t, core::AppError>
      read_extended_style() const noexcept
      {
        const auto style = read_window_long(window_, GWL_EXSTYLE);
        if (!style) {
          return std::unexpected{style.error()};
        }
        return static_cast<std::uintptr_t>(*style);
      }

      [[nodiscard]] std::expected<detail::LayeredAttributes, core::AppError>
      read_layered_attributes() const noexcept
      {
        COLORREF color_key{};
        BYTE alpha{};
        DWORD flags{};
        if (GetLayeredWindowAttributes(window_, &color_key, &alpha, &flags) ==
            FALSE) {
          return std::unexpected{last_platform_error()};
        }
        return detail::LayeredAttributes{
          .color_key = static_cast<std::uint32_t>(color_key),
          .alpha = alpha,
          .flags = static_cast<std::uint32_t>(flags),
        };
      }

      [[nodiscard]] std::expected<void, core::AppError>
      write_extended_style(const std::uintptr_t style) const noexcept
      {
        const auto result =
          write_window_long(window_, GWL_EXSTYLE, static_cast<LONG_PTR>(style));
        if (!result) {
          return std::unexpected{result.error()};
        }
        return {};
      }

      [[nodiscard]] std::expected<void, core::AppError> write_layered_attributes(
        const detail::LayeredAttributes attributes) const noexcept
      {
        if (SetLayeredWindowAttributes(
              window_, static_cast<COLORREF>(attributes.color_key),
              static_cast<BYTE>(attributes.alpha),
              static_cast<DWORD>(attributes.flags)) == FALSE) {
          return std::unexpected{last_platform_error()};
        }
        return {};
      }

    private:
      HWND window_{};
    };

    [[nodiscard]] LRESULT hit_zone_result(const detail::HitZone zone) noexcept
    {
      switch (zone) {
      case detail::HitZone::Transparent:
        return HTTRANSPARENT;
      case detail::HitZone::Caption:
        return HTCAPTION;
      case detail::HitZone::Left:
        return HTLEFT;
      case detail::HitZone::Right:
        return HTRIGHT;
      case detail::HitZone::Top:
        return HTTOP;
      case detail::HitZone::Bottom:
        return HTBOTTOM;
      case detail::HitZone::TopLeft:
        return HTTOPLEFT;
      case detail::HitZone::TopRight:
        return HTTOPRIGHT;
      case detail::HitZone::BottomLeft:
        return HTBOTTOMLEFT;
      case detail::HitZone::BottomRight:
        return HTBOTTOMRIGHT;
      case detail::HitZone::Delegate:
        return HTNOWHERE;
      }
      return HTNOWHERE;
    }

    [[nodiscard]] std::optional<detail::SizingEdge>
    sizing_edge(const WPARAM parameter) noexcept
    {
      switch (parameter) {
      case WMSZ_LEFT:
        return detail::SizingEdge::Left;
      case WMSZ_RIGHT:
        return detail::SizingEdge::Right;
      case WMSZ_TOP:
        return detail::SizingEdge::Top;
      case WMSZ_BOTTOM:
        return detail::SizingEdge::Bottom;
      case WMSZ_TOPLEFT:
        return detail::SizingEdge::TopLeft;
      case WMSZ_TOPRIGHT:
        return detail::SizingEdge::TopRight;
      case WMSZ_BOTTOMLEFT:
        return detail::SizingEdge::BottomLeft;
      case WMSZ_BOTTOMRIGHT:
        return detail::SizingEdge::BottomRight;
      default:
        return std::nullopt;
      }
    }

    [[nodiscard]] constexpr detail::NativeRect
    native_rect(const RECT &rect) noexcept
    {
      return {.left = rect.left,
              .top = rect.top,
              .right = rect.right,
              .bottom = rect.bottom};
    }

    [[nodiscard]] std::expected<detail::NativeRect, core::AppError>
    nearest_work_area(const HWND window) noexcept
    {
      const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
      if (monitor == nullptr) {
        return std::unexpected{last_platform_error()};
      }
      MONITORINFO info{.cbSize = sizeof(info)};
      if (GetMonitorInfoW(monitor, &info) == FALSE) {
        return std::unexpected{last_platform_error()};
      }
      return native_rect(info.rcWork);
    }

    [[nodiscard]] constexpr RECT
    win32_rect(const detail::NativeRect rect) noexcept
    {
      return {.left = rect.left,
              .top = rect.top,
              .right = rect.right,
              .bottom = rect.bottom};
    }

  }    // namespace

  struct Win32OverlayAdapter::PreparedWindowMessage final {
    enum class Kind { Result,
                      Sizing,
                      Dpi };

    Kind kind{Kind::Result};
    LRESULT result{};
    RECT *target_rect{};
    detail::NativeRect rect;
    std::uint32_t dpi{};
  };

  struct Win32OverlayAdapter::CallbackOperations final {
    Win32OverlayAdapter &adapter;

    [[nodiscard]] std::optional<PreparedWindowMessage>
    prepare(const UINT message, const WPARAM word_parameter, const LPARAM long_parameter)
    {
      return adapter.prepare_message(message, word_parameter, long_parameter);
    }

    [[nodiscard]] std::optional<LRESULT>
    commit(const PreparedWindowMessage &message) noexcept
    {
      return adapter.commit_message(message);
    }

    [[nodiscard]] bool
    forward_after_commit(const PreparedWindowMessage &message) const noexcept
    {
      return message.kind == PreparedWindowMessage::Kind::Dpi;
    }

    [[nodiscard]] LRESULT forward(const UINT message, const WPARAM word_parameter, const LPARAM long_parameter) noexcept
    {
      return adapter.forward_message(message, word_parameter, long_parameter);
    }

    void clear_binding() noexcept { adapter.clear_native_binding(); }
  };

  struct Win32OverlayAdapter::DetachOperations final {
    Win32OverlayAdapter &adapter;

    [[nodiscard]] std::expected<bool, core::AppError>
    restore_window_proc() noexcept
    {
      const auto restored = write_window_long(
        adapter.window_, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(adapter.saved_window_proc_));
      if (!restored) {
        return std::unexpected{restored.error()};
      }
      return *restored == reinterpret_cast<LONG_PTR>(&window_proc);
    }

    void publish_callback_detached() noexcept
    {
      adapter.attached_ = false;
      adapter.saved_window_proc_ = nullptr;
    }

    [[nodiscard]] detail::OverlayPropertyRemoval
    remove_and_observe_property() noexcept
    {
      return remove_and_observe_overlay_property(
        adapter.window_, reinterpret_cast<HANDLE>(&adapter));
    }

    void require_window_destruction() noexcept
    {
      adapter.window_destruction_required_ = true;
    }
  };

  std::expected<std::unique_ptr<Win32OverlayAdapter>, core::AppError>
  Win32OverlayAdapter::create_detached(OverlayWindow &overlay,
                                       const core::OverlayMode mode,
                                       const core::BorderPreference border,
                                       const core::Extent source_extent)
  {
    if (source_extent.width == 0 || source_extent.height == 0) {
      return std::unexpected{adapter_error(core::AppErrorCode::InvalidValue)};
    }
    auto *const glfw_window = overlay.native_window();
    if (glfw_window == nullptr) {
      return std::unexpected{
        adapter_error(core::AppErrorCode::PlatformOperationFailure)};
    }
    const auto window = glfwGetWin32Window(glfw_window);
    if (window == nullptr) {
      return std::unexpected{
        adapter_error(core::AppErrorCode::PlatformOperationFailure)};
    }
    auto dpi = GetDpiForWindow(window);
    if (dpi == 0) {
      dpi = 96;
    }
    return std::make_unique<Win32OverlayAdapter>(
      ConstructionToken{}, window, mode, border, source_extent, dpi);
  }

  Win32OverlayAdapter::Win32OverlayAdapter(ConstructionToken, const HWND window, const core::OverlayMode mode, const core::BorderPreference border, const core::Extent source_extent, const std::uint32_t dpi) noexcept
      : window_{window}, mode_{mode}, border_{border}, source_extent_{source_extent}, dpi_{dpi} {}

  Win32OverlayAdapter::~Win32OverlayAdapter() noexcept
  {
    if (attached_ || window_destruction_required_) {
      std::terminate();
    }
  }

  std::expected<void, core::AppError> Win32OverlayAdapter::attach() noexcept
  {
    if (attached_) {
      return {};
    }
    if (window_destruction_required_) {
      return std::unexpected{adapter_error(core::AppErrorCode::RuntimeStateLost)};
    }
    if (window_ == nullptr) {
      return std::unexpected{
        adapter_error(core::AppErrorCode::PlatformOperationFailure)};
    }

    SetLastError(ERROR_SUCCESS);
    const auto class_style = GetClassLongPtrW(window_, GCL_STYLE);
    if (class_style == 0 && GetLastError() != ERROR_SUCCESS) {
      return std::unexpected{last_platform_error()};
    }
    if (!detail::is_overlay_class_style_supported(
          static_cast<std::uintptr_t>(class_style))) {
      return std::unexpected{
        adapter_error(core::AppErrorCode::UnsupportedWindowClass,
                      static_cast<std::int64_t>(class_style))};
    }

    Win32StyleOperations style_operations{window_};
    std::optional<detail::NativeWindowState> initial_style;
    if (mode_ == core::OverlayMode::PassThrough) {
      const auto entered = detail::enter_pass_through(style_operations);
      if (!entered) {
        return std::unexpected{entered.error()};
      }
      initial_style = *entered;
    }

    const auto rollback_style = [&]() noexcept {
      if (!initial_style) {
        return true;
      }
      static_cast<void>(
        detail::apply_native_window_state(style_operations, *initial_style));
      return detail::native_window_state_matches(style_operations,
                                                 *initial_style);
    };
    const auto attachment_failure = [&](const bool native_binding_safe) noexcept {
      const auto style_safe = rollback_style();
      return adapter_error(native_binding_safe && style_safe
                             ? core::AppErrorCode::PlatformOperationFailure
                             : core::AppErrorCode::RuntimeStateLost);
    };

    const auto saved_proc = read_window_long(window_, GWLP_WNDPROC);
    if (!saved_proc) {
      return std::unexpected{attachment_failure(true)};
    }
    if (*saved_proc == 0) {
      return std::unexpected{attachment_failure(false)};
    }
    if (GetPropW(window_, overlay_adapter_property) != nullptr) {
      return std::unexpected{attachment_failure(true)};
    }
    saved_window_proc_ = reinterpret_cast<WNDPROC>(*saved_proc);
    if (SetPropW(window_, overlay_adapter_property,
                 reinterpret_cast<HANDLE>(this)) == FALSE) {
      saved_window_proc_ = nullptr;
      return std::unexpected{attachment_failure(true)};
    }

    const auto installed = write_window_long(
      window_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc));
    if (!installed) {
      const auto observed_proc = read_window_long(window_, GWLP_WNDPROC);
      auto procedure = detail::FailedWndProcInstallObservation::Unavailable;
      if (observed_proc && *observed_proc == *saved_proc) {
        procedure = detail::FailedWndProcInstallObservation::Original;
      }
      else if (observed_proc &&
               *observed_proc == reinterpret_cast<LONG_PTR>(&window_proc)) {
        procedure = detail::FailedWndProcInstallObservation::Custom;
      }

      auto property = detail::OverlayPropertyRemoval::StillPresent;
      if (procedure == detail::FailedWndProcInstallObservation::Original) {
        property = remove_and_observe_overlay_property(
          window_, reinterpret_cast<HANDLE>(this));
      }
      else if (GetPropW(window_, overlay_adapter_property) == nullptr) {
        property = detail::OverlayPropertyRemoval::AbsentUnexpected;
      }
      const auto cleanup = detail::failed_attachment_cleanup(procedure, property);
      if (cleanup.destroy_with_adapter_alive) {
        window_destruction_required_ = true;
      }
      else {
        saved_window_proc_ = nullptr;
      }
      return std::unexpected{attachment_failure(cleanup.native_binding_safe)};
    }

    saved_window_proc_ = reinterpret_cast<WNDPROC>(*installed);
    pass_through_snapshot_ = initial_style;
    attached_ = true;
    return {};
  }

  std::expected<void, core::AppError> Win32OverlayAdapter::detach() noexcept
  {
    if (!attached_) {
      return window_destruction_required_
               ? std::unexpected{adapter_error(
                   core::AppErrorCode::RuntimeStateLost)}
               : std::expected<void, core::AppError>{};
    }
    if (window_ == nullptr || saved_window_proc_ == nullptr) {
      return std::unexpected{adapter_error(core::AppErrorCode::RuntimeStateLost)};
    }
    DetachOperations operations{.adapter = *this};
    return detail::detach_overlay_subclass(operations);
  }

  bool Win32OverlayAdapter::attached() const noexcept { return attached_; }

  bool Win32OverlayAdapter::window_destruction_required() const noexcept
  {
    return window_destruction_required_;
  }

  void Win32OverlayAdapter::complete_after_overlay_destroyed() noexcept
  {
    if (!window_destruction_required_) {
      return;
    }
    attached_ = false;
    window_destruction_required_ = false;
    saved_window_proc_ = nullptr;
    pass_through_snapshot_.reset();
    window_ = nullptr;
  }

  std::expected<void, core::AppError>
  Win32OverlayAdapter::set_mode(const core::OverlayMode mode) noexcept
  {
    if (mode == mode_) {
      return {};
    }
    if (window_ == nullptr) {
      return std::unexpected{
        adapter_error(core::AppErrorCode::PlatformOperationFailure)};
    }

    Win32StyleOperations operations{window_};
    if (mode == core::OverlayMode::PassThrough) {
      const auto entered = detail::enter_pass_through(operations);
      if (!entered) {
        return std::unexpected{entered.error()};
      }
      pass_through_snapshot_ = *entered;
      mode_ = mode;
      return {};
    }
    if (!pass_through_snapshot_) {
      return std::unexpected{adapter_error(core::AppErrorCode::RuntimeStateLost)};
    }
    const auto left =
      detail::leave_pass_through(operations, *pass_through_snapshot_);
    if (!left) {
      return std::unexpected{left.error()};
    }
    pass_through_snapshot_.reset();
    mode_ = mode;
    return {};
  }

  core::OverlayMode Win32OverlayAdapter::mode() const noexcept { return mode_; }

  std::expected<OverlayClientGeometry, core::AppError>
  Win32OverlayAdapter::snapshot_client_geometry() const noexcept
  {
    if (window_ == nullptr) {
      return std::unexpected{
        adapter_error(core::AppErrorCode::PlatformOperationFailure)};
    }
    RECT client{};
    if (GetClientRect(window_, &client) == FALSE) {
      return std::unexpected{last_platform_error()};
    }
    POINT upper_left{.x = client.left, .y = client.top};
    POINT lower_right{.x = client.right, .y = client.bottom};
    if (ClientToScreen(window_, &upper_left) == FALSE ||
        ClientToScreen(window_, &lower_right) == FALSE) {
      return std::unexpected{last_platform_error()};
    }
    return OverlayClientGeometry{.client_screen_rect = {
                                   .left = upper_left.x,
                                   .top = upper_left.y,
                                   .right = lower_right.x,
                                   .bottom = lower_right.y,
                                 }};
  }

  std::expected<void, core::AppError>
  Win32OverlayAdapter::restore_client_geometry(
    const OverlayClientGeometry &snapshot) noexcept
  {
    if (window_ == nullptr) {
      return std::unexpected{
        adapter_error(core::AppErrorCode::PlatformOperationFailure)};
    }
    const auto content_width =
      static_cast<std::int64_t>(snapshot.client_screen_rect.right) -
      snapshot.client_screen_rect.left;
    const auto content_height =
      static_cast<std::int64_t>(snapshot.client_screen_rect.bottom) -
      snapshot.client_screen_rect.top;
    if (content_width <= 0 || content_height <= 0) {
      return std::unexpected{adapter_error(core::AppErrorCode::InvalidValue)};
    }
    const auto insets = frame_insets(window_, dpi_);
    if (!insets) {
      return std::unexpected{insets.error()};
    }
    const auto work_area = nearest_work_area(window_);
    if (!work_area) {
      return std::unexpected{work_area.error()};
    }
    const auto requested_outer =
      detail::content_to_outer_rect(snapshot.client_screen_rect, *insets);
    const auto outer =
      detail::clamp_outer_rect_to_work_area(requested_outer, *work_area);
    const auto outer_width = static_cast<std::int64_t>(outer.right) - outer.left;
    const auto outer_height = static_cast<std::int64_t>(outer.bottom) - outer.top;
    if (outer_width <= 0 || outer_height <= 0 ||
        outer_width > (std::numeric_limits<int>::max)() ||
        outer_height > (std::numeric_limits<int>::max)()) {
      return std::unexpected{adapter_error(core::AppErrorCode::InvalidValue)};
    }
    if (SetWindowPos(window_, nullptr, outer.left, outer.top,
                     static_cast<int>(outer_width),
                     static_cast<int>(outer_height),
                     SWP_NOACTIVATE | SWP_NOZORDER) == FALSE) {
      return std::unexpected{last_platform_error()};
    }
    const OverlayClientGeometry expected{
      .client_screen_rect = detail::outer_to_content_rect(outer, *insets)};
    if (!client_geometry_matches(expected)) {
      return std::unexpected{
        adapter_error(core::AppErrorCode::PlatformOperationFailure)};
    }
    return {};
  }

  bool Win32OverlayAdapter::client_geometry_matches(
    const OverlayClientGeometry &snapshot) const noexcept
  {
    const auto observed = snapshot_client_geometry();
    return observed && *observed == snapshot;
  }

  void Win32OverlayAdapter::set_border_preference(
    const core::BorderPreference border) noexcept
  {
    border_ = border;
  }

  core::BorderPreference Win32OverlayAdapter::border_preference() const noexcept
  {
    return border_;
  }

  void Win32OverlayAdapter::update_source_extent(
    const core::Extent source_extent) noexcept
  {
    if (source_extent.width != 0 && source_extent.height != 0) {
      source_extent_ = source_extent;
    }
  }

  std::optional<Win32OverlayAdapter::PreparedWindowMessage>
  Win32OverlayAdapter::prepare_message(const UINT message,
                                       const WPARAM word_parameter,
                                       const LPARAM long_parameter)
  {
    if (message == WM_NCHITTEST) {
      RECT rect{};
      if (GetWindowRect(window_, &rect) == FALSE) {
        return std::nullopt;
      }
      const auto zone = detail::overlay_hit_test(
        mode_, border_, native_rect(rect),
        {.x = GET_X_LPARAM(long_parameter), .y = GET_Y_LPARAM(long_parameter)},
        detail::scaled_resize_band(logical_resize_band, dpi_));
      if (zone == detail::HitZone::Delegate) {
        return std::nullopt;
      }
      return PreparedWindowMessage{.kind = PreparedWindowMessage::Kind::Result,
                                   .result = hit_zone_result(zone)};
    }
    if (message == WM_SIZING) {
      const auto edge = sizing_edge(word_parameter);
      auto *const rect = reinterpret_cast<RECT *>(long_parameter);
      const auto insets = frame_insets(window_, dpi_);
      if (!edge || rect == nullptr || !insets) {
        return std::nullopt;
      }
      return PreparedWindowMessage{
        .kind = PreparedWindowMessage::Kind::Sizing,
        .result = TRUE,
        .target_rect = rect,
        .rect = detail::aspect_constrained_rect(native_rect(*rect), *edge,
                                                source_extent_, *insets),
      };
    }
    if (message == WM_DPICHANGED) {
      auto *const rect = reinterpret_cast<RECT *>(long_parameter);
      return PreparedWindowMessage{
        .kind = PreparedWindowMessage::Kind::Dpi,
        .target_rect = rect,
        .rect = rect == nullptr ? detail::NativeRect{} : native_rect(*rect),
        .dpi = LOWORD(word_parameter),
      };
    }
    return std::nullopt;
  }

  std::optional<LRESULT> Win32OverlayAdapter::commit_message(
    const PreparedWindowMessage &message) noexcept
  {
    switch (message.kind) {
    case PreparedWindowMessage::Kind::Result:
      return message.result;
    case PreparedWindowMessage::Kind::Sizing:
      if (message.target_rect == nullptr) {
        return std::nullopt;
      }
      *message.target_rect = win32_rect(message.rect);
      return message.result;
    case PreparedWindowMessage::Kind::Dpi:
      if (message.target_rect == nullptr) {
        return std::nullopt;
      }
      if (!detail::apply_dpi_change_transaction(
            dpi_, message.dpi, message.rect,
            [this](const int left, const int top, const int width,
                   const int height) noexcept {
              return SetWindowPos(window_, nullptr, left, top, width, height,
                                  SWP_NOACTIVATE | SWP_NOZORDER) != FALSE;
            })) {
        return std::nullopt;
      }
      return 0;
    }
    return std::nullopt;
  }

  LRESULT
  Win32OverlayAdapter::forward_message(const UINT message,
                                       const WPARAM word_parameter,
                                       const LPARAM long_parameter) noexcept
  {
    if (saved_window_proc_ == nullptr) {
      return DefWindowProcW(window_, message, word_parameter, long_parameter);
    }
    return CallWindowProcW(saved_window_proc_, window_, message, word_parameter,
                           long_parameter);
  }

  void Win32OverlayAdapter::clear_native_binding() noexcept
  {
    if (window_ != nullptr && GetPropW(window_, overlay_adapter_property) ==
                                reinterpret_cast<HANDLE>(this)) {
      static_cast<void>(RemovePropW(window_, overlay_adapter_property));
    }
    attached_ = false;
    window_destruction_required_ = false;
    saved_window_proc_ = nullptr;
    pass_through_snapshot_.reset();
    window_ = nullptr;
  }

  LRESULT CALLBACK Win32OverlayAdapter::window_proc(
    const HWND window, const UINT message, const WPARAM word_parameter, const LPARAM long_parameter) noexcept
  {
    auto *const adapter = reinterpret_cast<Win32OverlayAdapter *>(
      GetPropW(window, overlay_adapter_property));
    if (adapter == nullptr) {
      return DefWindowProcW(window, message, word_parameter, long_parameter);
    }
    CallbackOperations operations{.adapter = *adapter};
    return detail::dispatch_overlay_callback(operations, message, word_parameter,
                                             long_parameter);
  }

}    // namespace airelinna::platform::windows
