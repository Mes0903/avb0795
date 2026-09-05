#ifndef AIRELINNA_PLATFORM_WINDOWS_WIN32_OVERLAY_ADAPTER_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WIN32_OVERLAY_ADAPTER_HPP

#include "airelinna/core/types.hpp"
#include "platform/windows/detail/overlay_policy.hpp"
#include "platform/windows/detail/overlay_transactions.hpp"

#include <windows.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace airelinna::platform::windows {

class OverlayWindow;

struct OverlayClientGeometry final {
  detail::NativeRect client_screen_rect;

  friend bool operator==(const OverlayClientGeometry &,
                         const OverlayClientGeometry &) = default;
};

class Win32OverlayAdapter final {
private:
  struct ConstructionToken final {
  private:
    ConstructionToken() = default;
    friend class Win32OverlayAdapter;
  };

  struct PreparedWindowMessage;
  struct CallbackOperations;
  struct DetachOperations;

public:
  [[nodiscard]] static std::expected<std::unique_ptr<Win32OverlayAdapter>,
                                     core::AppError>
  create_detached(OverlayWindow &overlay, core::OverlayMode mode,
                  core::BorderPreference border, core::Extent source_extent);

  Win32OverlayAdapter(ConstructionToken, HWND window, core::OverlayMode mode,
                      core::BorderPreference border, core::Extent source_extent,
                      std::uint32_t dpi) noexcept;
  ~Win32OverlayAdapter() noexcept;

  Win32OverlayAdapter(const Win32OverlayAdapter &) = delete;
  Win32OverlayAdapter &operator=(const Win32OverlayAdapter &) = delete;
  Win32OverlayAdapter(Win32OverlayAdapter &&) = delete;
  Win32OverlayAdapter &operator=(Win32OverlayAdapter &&) = delete;

  [[nodiscard]] std::expected<void, core::AppError> attach() noexcept;
  [[nodiscard]] std::expected<void, core::AppError> detach() noexcept;
  [[nodiscard]] bool attached() const noexcept;
  [[nodiscard]] bool window_destruction_required() const noexcept;
  void complete_after_overlay_destroyed() noexcept;

  [[nodiscard]] std::expected<void, core::AppError>
  set_mode(core::OverlayMode mode) noexcept;
  [[nodiscard]] core::OverlayMode mode() const noexcept;

  [[nodiscard]] std::expected<OverlayClientGeometry, core::AppError>
  snapshot_client_geometry() const noexcept;
  [[nodiscard]] std::expected<void, core::AppError>
  restore_client_geometry(const OverlayClientGeometry &snapshot) noexcept;
  [[nodiscard]] bool
  client_geometry_matches(const OverlayClientGeometry &snapshot) const noexcept;

  void set_border_preference(core::BorderPreference border) noexcept;
  [[nodiscard]] core::BorderPreference border_preference() const noexcept;
  void update_source_extent(core::Extent source_extent) noexcept;

private:
  static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                      WPARAM word_parameter,
                                      LPARAM long_parameter) noexcept;

  [[nodiscard]] std::optional<PreparedWindowMessage>
  prepare_message(UINT message, WPARAM word_parameter, LPARAM long_parameter);
  [[nodiscard]] std::optional<LRESULT>
  commit_message(const PreparedWindowMessage &message) noexcept;
  [[nodiscard]] LRESULT forward_message(UINT message, WPARAM word_parameter,
                                        LPARAM long_parameter) noexcept;
  void clear_native_binding() noexcept;

  HWND window_{};
  WNDPROC saved_window_proc_{};
  core::OverlayMode mode_{core::OverlayMode::Editable};
  core::BorderPreference border_{core::BorderPreference::Bordered};
  core::Extent source_extent_{};
  std::uint32_t dpi_{96};
  std::optional<detail::NativeWindowState> pass_through_snapshot_;
  bool attached_{};
  bool window_destruction_required_{};
};

} // namespace airelinna::platform::windows

#endif // AIRELINNA_PLATFORM_WINDOWS_WIN32_OVERLAY_ADAPTER_HPP
