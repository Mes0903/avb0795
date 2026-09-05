/// @file
/// @brief Win32 overlay subclass, input-transparency transactions, and DPI geometry.
/// Native implementation details remain below the portable application ports.

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

  /// Client rectangle in screen coordinates, independent of current border chrome.
  struct OverlayClientGeometry final {
    detail::NativeRect client_screen_rect;

    friend bool operator==(const OverlayClientGeometry &,
                           const OverlayClientGeometry &) = default;
  };

  /// Main-thread borrower of the overlay HWND and owner of its subclass binding.
  /// Manages input style, DPI, and geometry; never toggles GLFW decoration itself.
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
    /// Resolve the native window/initial DPI and allocate stable state before subclassing.
    [[nodiscard]] static std::expected<std::unique_ptr<Win32OverlayAdapter>,
                                       core::AppError>
    create_detached(OverlayWindow &overlay, core::OverlayMode mode, core::BorderPreference border, core::Extent source_extent);

    Win32OverlayAdapter(ConstructionToken, HWND window, core::OverlayMode mode, core::BorderPreference border, core::Extent source_extent, std::uint32_t dpi) noexcept;
    ~Win32OverlayAdapter() noexcept;

    Win32OverlayAdapter(const Win32OverlayAdapter &) = delete;
    Win32OverlayAdapter &operator=(const Win32OverlayAdapter &) = delete;
    Win32OverlayAdapter(Win32OverlayAdapter &&) = delete;
    Win32OverlayAdapter &operator=(Win32OverlayAdapter &&) = delete;

    /// Install the custom procedure only after callback-visible state is complete.
    [[nodiscard]] std::expected<void, core::AppError> attach() noexcept;
    /// Restore the saved GLFW procedure and verify removal of the native binding.
    [[nodiscard]] std::expected<void, core::AppError> detach() noexcept;
    /// Whether the custom procedure is still attached to the live window.
    [[nodiscard]] bool attached() const noexcept;
    /// Whether cleanup must destroy HWND while this adapter remains alive.
    [[nodiscard]] bool window_destruction_required() const noexcept;
    /// Finish fail-safe cleanup only after window destruction removed callback reachability.
    void complete_after_overlay_destroyed() noexcept;

    /// Enter/leave opaque layered Pass-through with exact native-state rollback.
    [[nodiscard]] std::expected<void, core::AppError>
    set_mode(core::OverlayMode mode) noexcept;
    /// Return the confirmed mode used by hit-test/message policy.
    [[nodiscard]] core::OverlayMode mode() const noexcept;

    /// Snapshot content placement before changing GLFW's border attributes.
    [[nodiscard]] std::expected<OverlayClientGeometry, core::AppError>
    snapshot_client_geometry() const noexcept;
    /// Restore content placement using current frame insets and monitor work area.
    [[nodiscard]] std::expected<void, core::AppError>
    restore_client_geometry(const OverlayClientGeometry &snapshot) noexcept;
    /// Verify the same clamped geometry used by restoration.
    [[nodiscard]] bool
    client_geometry_matches(const OverlayClientGeometry &snapshot) const noexcept;

    /// Update message-policy state after the separate GLFW border operation.
    void set_border_preference(core::BorderPreference border) noexcept;
    /// Cached border policy; native decoration ownership remains in OverlayWindow.
    [[nodiscard]] core::BorderPreference border_preference() const noexcept;
    /// Update the aspect constraint when runtime accepts a new capture allocation.
    void update_source_extent(core::Extent source_extent) noexcept;

  private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM word_parameter, LPARAM long_parameter) noexcept;

    [[nodiscard]] std::optional<PreparedWindowMessage>
    prepare_message(UINT message, WPARAM word_parameter, LPARAM long_parameter);
    [[nodiscard]] std::optional<LRESULT>
    commit_message(const PreparedWindowMessage &message) noexcept;
    [[nodiscard]] LRESULT forward_message(UINT message, WPARAM word_parameter, LPARAM long_parameter) noexcept;
    void clear_native_binding() noexcept;

    // The window property points at this stable object while the subclass is live.
    // Normal detach clears it; failure cleanup forwards WM_NCDESTROY first.
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

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WIN32_OVERLAY_ADAPTER_HPP
