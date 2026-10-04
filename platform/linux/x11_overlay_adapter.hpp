/// @file
/// @brief Main-thread X11 replica attachment and bounded native facts.
#ifndef AVB0795_LINUX_X11_OVERLAY_ADAPTER_HPP
#define AVB0795_LINUX_X11_OVERLAY_ADAPTER_HPP
#include "avb/core/types.hpp"
#include "render/replica_presentation.hpp"
#include <expected>
#include <memory>
struct GLFWwindow;
namespace avb::linux_platform {
  /// @brief Coalesced owner facts; resize is drawable geometry, never source content.
  /// A terminal region fact with no region is cancellation. Preview is newest-only
  /// renderer input and never changes the accepted crop.
  struct X11OverlayEvents {
    bool close_requested{};
    std::optional<core::Extent> resized;
    bool region_finished{};
    std::optional<core::PixelRegion> region;
    std::optional<core::AppError> failure;
  };
  /// @brief Borrow a standard GLFW X11 window with initially default input Shape.
  /// The caller retains window ownership and destroys this attachment first, on
  /// the GLFW main thread. Callbacks preserve predecessors and reserve no user
  /// pointer. apply accepts a complete candidate only after native verification;
  /// mapped WM requests remain asynchronous. Failed restoration reports state loss.
  /// Only the current renderer's retirement-filtered snapshots may be supplied.
  /// Zero content/framebuffer invalidates mapping and resets sequence admission.
  class X11OverlayAdapter final {
  public:
    static std::expected<std::unique_ptr<X11OverlayAdapter>, core::AppError> create(GLFWwindow *);
    ~X11OverlayAdapter();
    std::expected<void, core::AppError> apply(const core::ReplicaSettings &) noexcept;
    void set_presentation(render::PresentationSnapshot) noexcept;
    [[nodiscard]] X11OverlayEvents take_events() noexcept;
    /// @brief Report only window-interaction prerequisites for this borrowed window.
    /// Only pass_through, borderless_resize, always_on_top and window_opacity are
    /// meaningful. Composition obtains capture and selection capabilities from
    /// its source catalog independently; this broad value supplies no source authority.
    [[nodiscard]] core::PlatformCapabilities capabilities() const noexcept;
    [[nodiscard]] std::optional<core::PixelRegion> selection_preview() const noexcept;
    X11OverlayAdapter(const X11OverlayAdapter &) = delete;
    X11OverlayAdapter &operator=(const X11OverlayAdapter &) = delete;

  private:
    struct Impl;
    explicit X11OverlayAdapter(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
  };
}    // namespace avb::linux_platform
#endif
