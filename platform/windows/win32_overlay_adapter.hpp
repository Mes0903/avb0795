/// @file
/// @brief Win32 overlay subclass, input-transparency transactions, and DPI geometry.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_WIN32_OVERLAY_ADAPTER_HPP
#define AVB0795_PLATFORM_WINDOWS_WIN32_OVERLAY_ADAPTER_HPP

#include "avb/core/types.hpp"
#include "platform/windows/detail/overlay_policy.hpp"
#include "platform/windows/detail/overlay_transactions.hpp"

#include <windows.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace avb::platform::windows {

  class OverlayWindow;

  /// Client rectangle in screen coordinates, independent of current border chrome.
  struct OverlayClientGeometry final {
    detail::NativeRect client_screen_rect;

    friend bool operator==(const OverlayClientGeometry &,
                           const OverlayClientGeometry &) = default;
  };

  /// Forward border changes use work-area bounds; rollback restores the exact snapshot.
  enum class GeometryRestoreBounds { WorkArea, Exact };

  /// Drag endpoints in physical client pixels, bounded by the displayed content.
  struct OverlayRegionGesture final {
    detail::NativePoint start;
    detail::NativePoint end;

    friend bool operator==(const OverlayRegionGesture &,
                           const OverlayRegionGesture &) = default;
  };

  /// One terminal fact; an empty gesture means cancellation, not a crop reset.
  struct OverlayRegionSelectionResult final {
    std::optional<OverlayRegionGesture> gesture;
  };

  /// @brief Own the native callback binding while borrowing the overlay window.
  ///
  /// Main-thread operations manage input style, DPI, and client geometry. The
  /// window property points to this immovable object, so it must stay alive until
  /// verified detach or final window destruction removes callback reachability.
  /// Destruction with a live or uncertain binding terminates rather than leaving
  /// a dangling native callback. Decoration remains the window owner's concern.
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

    /// @brief Install the subclass after its property and saved procedure are ready.
    ///
    /// A failed native setter does not prove installation had no effect. Failure
    /// handling observes the procedure and property; callers must honor
    /// window_destruction_required() even when attach itself returned an error.
    [[nodiscard]] std::expected<void, core::AppError> attach() noexcept;
    /// @brief Restore the previous procedure before removing callback context.
    ///
    /// A failed result requires cleanup while this adapter remains alive. The
    /// owner can destroy the window after graphics retirement, allowing its final
    /// message to clear the binding, then complete fallback cleanup explicitly.
    [[nodiscard]] std::expected<void, core::AppError> detach() noexcept;
    /// Whether the custom procedure is still attached to the live window.
    [[nodiscard]] bool attached() const noexcept;
    /// Whether cleanup must destroy HWND while this adapter remains alive.
    [[nodiscard]] bool window_destruction_required() const noexcept;
    /// Finish fail-safe cleanup only after window destruction removed callback reachability.
    void complete_after_overlay_destroyed() noexcept;

    /// @brief Apply whole-window alpha and input mode together, preserving decoration bits.
    /// Percent must be 0..100. Observed native readback commits the cached pair;
    /// a failed operation restores its exact current snapshot or reports RuntimeStateLost.
    [[nodiscard]] std::expected<void, core::AppError>
    set_presentation(core::OverlayMode effective_mode, int opacity_percent) noexcept;
    /// Change input mode while preserving the confirmed whole-window opacity.
    [[nodiscard]] std::expected<void, core::AppError>
    set_mode(core::OverlayMode mode) noexcept;
    /// Return the confirmed mode used by hit-test/message policy.
    [[nodiscard]] core::OverlayMode mode() const noexcept;
    /// Return the confirmed opacity percentage, independent of mode and border.
    [[nodiscard]] int opacity_percent() const noexcept;
    /// Read back native input/alpha state after independent border transactions.
    [[nodiscard]] bool presentation_matches(core::OverlayMode mode,
                                             int opacity_percent) const noexcept;

    /// @brief Begin, arm, or end a bounded client-coordinate selection cycle.
    /// Active with no content rectangle waits for a matching full-view present.
    /// A rectangle arms input; repeated active calls cannot rearm a terminal cycle.
    /// Ending clears pending facts before releasing capture. No preferences change.
    void set_region_selection(
      bool active, std::optional<detail::NativeRect> content_rect = std::nullopt) noexcept;
    /// Activate only in response to the user's begin command; observe actual focus.
    [[nodiscard]] bool focus_region_selection() noexcept;
    /// Latest bounded endpoints; source-pixel conversion belongs to the runtime.
    [[nodiscard]] std::optional<OverlayRegionGesture>
    region_selection_preview() const noexcept;
    /// Consume the single completion/cancellation fact; input remains frozen.
    [[nodiscard]] std::optional<OverlayRegionSelectionResult>
    take_region_selection_result() noexcept;

    /// Snapshot content placement before changing GLFW's border attributes.
    [[nodiscard]] std::expected<OverlayClientGeometry, core::AppError>
    snapshot_client_geometry() const noexcept;
    /// @brief Fit baseline client width and return the verified client geometry.
    ///
    /// Current frame insets and the nearest work area bound the content aspect.
    /// The native move preserves activation/Z order and requires exact readback.
    /// Programmatic fit bypasses the interactive minimum tracking dimensions.
    /// Invalid inputs or native failure leave rollback to the calling transaction.
    [[nodiscard]] std::expected<OverlayClientGeometry, core::AppError>
    fit_client_geometry(const OverlayClientGeometry &baseline, core::Extent source) noexcept;
    /// @brief Restore client placement with current border insets and the selected bounds.
    ///
    /// The outer rectangle is clamped to the nearest monitor's work area, so the
    /// resulting client rectangle can differ from the original snapshot. Exact
    /// skips that clamp and verifies the original snapshot for transaction rollback.
    /// Unrepresentable outer geometry is rejected before moving; failed native
    /// operations/readback report an error and require caller-owned rollback.
    /// Programmatic placement bypasses interactive tracking minimum dimensions.
    [[nodiscard]] std::expected<void, core::AppError>
    restore_client_geometry(const OverlayClientGeometry &snapshot,
      GeometryRestoreBounds bounds = GeometryRestoreBounds::WorkArea) noexcept;
    /// @brief Compare native client geometry with the supplied rectangle exactly.
    ///
    /// No clamping is performed here. A caller checking an original snapshot
    /// after a clamped restoration must account for the changed rectangle.
    [[nodiscard]] bool
    client_geometry_matches(const OverlayClientGeometry &snapshot) const noexcept;

    /// Update message-policy state after the separate GLFW border operation.
    void set_border_preference(core::BorderPreference border) noexcept;
    /// Cached border policy; native decoration ownership remains in OverlayWindow.
    [[nodiscard]] core::BorderPreference border_preference() const noexcept;
    /// Update the aspect constraint from the runtime's effective presentation extent.
    void update_source_extent(core::Extent source_extent) noexcept;
    /// Install an accepted 1..25 percent wheel step, clearing partial input on change.
    void set_wheel_zoom_percent(int percent) noexcept;

  private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM word_parameter, LPARAM long_parameter) noexcept;

    [[nodiscard]] std::optional<PreparedWindowMessage>
    prepare_message(UINT message, WPARAM word_parameter, LPARAM long_parameter);
    [[nodiscard]] std::optional<LRESULT>
    commit_message(const PreparedWindowMessage &message) noexcept;
    [[nodiscard]] LRESULT forward_message(UINT message, WPARAM word_parameter, LPARAM long_parameter) noexcept;
    void cancel_region_selection() noexcept;
    void release_region_capture() noexcept;
    void clear_native_binding() noexcept;

    // The window property points at this stable object while the subclass is live.
    // Normal detach clears it; failure cleanup forwards WM_NCDESTROY first.
    HWND window_{};
    WNDPROC saved_window_proc_{};
    core::OverlayMode mode_{core::OverlayMode::Editable};
    int opacity_percent_{100};
    int wheel_zoom_percent_{core::default_wheel_zoom_percent};
    int wheel_remainder_{};
    core::BorderPreference border_{core::BorderPreference::Bordered};
    core::Extent source_extent_{};
    std::uint32_t dpi_{96};
    bool attached_{};
    bool window_destruction_required_{};
    bool region_selection_active_{};
    bool region_selection_terminal_{};
    bool region_selection_focus_observed_{};
    std::optional<detail::NativeRect> region_content_rect_;
    std::optional<OverlayRegionGesture> region_gesture_;
    std::optional<OverlayRegionSelectionResult> region_result_;
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WIN32_OVERLAY_ADAPTER_HPP
