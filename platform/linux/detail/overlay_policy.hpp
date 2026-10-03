/// @file
/// @brief Linux client geometry, settings acceptance and retired selection mapping.
#ifndef AVB0795_LINUX_OVERLAY_POLICY_HPP
#define AVB0795_LINUX_OVERLAY_POLICY_HPP
#include "avb/core/types.hpp"
#include "render/replica_presentation.hpp"
#include <expected>
namespace avb::linux_platform::detail {
  struct ClientRect {
    int x{}, y{}, width{}, height{};
    friend bool operator==(const ClientRect &, const ClientRect &) = default;
  };
  struct Insets {
    int left{}, top{}, right{}, bottom{};
  };
  /// @brief EWMH directions, with no gesture for client controls outside the title.
  [[nodiscard]] int hit_direction(double x, double y, core::Extent client, float scale, float title_right, float title_bottom) noexcept;
  /// @brief Preserve client center and cropped aspect; work-area capacity overrides scaled minima.
  [[nodiscard]] std::optional<ClientRect> wheel_geometry(ClientRect, core::Extent aspect, ClientRect work, Insets, float scale, int notches, int percent) noexcept;
  /// @brief A single concrete settings transaction, not a per-request native facade.
  /// Snapshot precedes all mutations. Mutation success means properties verified and
  /// asynchronous WM requests accepted; restore verifies properties and compensation.
  /// Callbacks borrow context only during transact, on the owning main thread.
  struct SettingsOperations {
    void *context{};
    std::expected<void, core::AppError> (*snapshot)(void *){};
    std::expected<void, core::AppError> (*mutate)(void *, const core::ReplicaSettings &){};
    bool (*restore)(void *){};
  };
  [[nodiscard]] std::expected<void, core::AppError> transact(
    core::ReplicaSettings &accepted, const core::ReplicaSettings &, SettingsOperations) noexcept;
  /// @brief Fixed gesture mapping from already retired full-view presentations.
  /// Zero geometry discards sequence history so replacement renderers can restart
  /// at one. Content changes end armed and dragging selections; view-only changes
  /// before press preserve the pending full-view transition. Dragging geometry
  /// changes cancel instead of remapping endpoints. Native owners must commit
  /// restored accepted preferences before publishing a terminal result.
  class SelectionMapping {
  public:
    void begin() noexcept;
    void cancel() noexcept;
    void publish(render::PresentationSnapshot, core::Extent client) noexcept;
    void press(double x, double y) noexcept;
    void move(double x, double y) noexcept;
    void release(double x, double y) noexcept;
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool terminal() const noexcept { return terminal_; }
    [[nodiscard]] std::optional<core::PixelRegion> result() const noexcept { return result_; }
    [[nodiscard]] std::optional<core::PixelRegion> preview() const noexcept { return preview_; }
    [[nodiscard]] std::optional<render::PresentationSnapshot> snapshot() const noexcept { return snapshot_; }

  private:
    [[nodiscard]] render::PixelPoint point(double, double) const noexcept;
    std::optional<render::PresentationSnapshot> snapshot_;
    core::Extent client_{};
    render::PixelPoint start_{};
    std::optional<core::PixelRegion> preview_, result_;
    std::uint64_t sequence_{}, arm_sequence_{};
    bool active_{}, dragging_{}, terminal_{};
  };
}    // namespace avb::linux_platform::detail
#endif
