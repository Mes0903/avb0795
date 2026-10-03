/// @file
/// @brief Checked Linux geometry and atomic logical acceptance after native verification.
#include "platform/linux/detail/overlay_policy.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace avb::linux_platform::detail {
  int hit_direction(double x, double y, core::Extent client, float scale, float right, float bottom) noexcept
  {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(scale) || scale <= 0 || !client.width || !client.height || x < 0 || y < 0 || x >= client.width || y >= client.height) return -1;
    const double bx = std::min(8.0 * scale, client.width / 2.0), by = std::min(8.0 * scale, client.height / 2.0);
    const bool l = x < bx, r = x >= client.width - bx, t = y < by, b = y >= client.height - by;
    if (t && l) return 0;
    if (t && r) return 2;
    if (b && r) return 4;
    if (b && l) return 6;
    if (t) return 1;
    if (r) return 3;
    if (b) return 5;
    if (l) return 7;
    return std::isfinite(right) && std::isfinite(bottom) && x < client.width * std::clamp(right, 0.f, 1.f) && y < client.height * std::clamp(bottom, 0.f, 1.f) ? 8 : -1;
  }
  std::optional<ClientRect> wheel_geometry(ClientRect c, core::Extent aspect, ClientRect work, Insets i, float scale, int notches, int percent) noexcept
  {
    if (c.width <= 0 || c.height <= 0 || work.width <= 0 || work.height <= 0 || !aspect.width || !aspect.height || !std::isfinite(scale) || scale <= 0 || !core::valid_wheel_zoom_percent(percent) || i.left < 0 || i.right < 0 || i.top < 0 || i.bottom < 0) return {};
    const double aw = static_cast<double>(work.width) - i.left - i.right, ah = static_cast<double>(work.height) - i.top - i.bottom;
    if (aw < 1 || ah < 1) return {};
    const double ratio = static_cast<double>(aspect.width) / aspect.height;
    const double maximum = std::min(aw, ah * ratio), minimum = std::min(maximum, std::max(160.0 * scale, 90.0 * scale * ratio));
    const double factor = std::pow(1.0 + percent / 100.0, static_cast<double>(notches));
    if (std::isnan(factor)) return {};
    const double w = std::clamp(std::round(std::clamp(c.width * factor, minimum, maximum)), 1.0, aw);
    const double h = std::min(ah, std::max(1.0, std::round(w / ratio)));
    const double minx = static_cast<double>(work.x) + i.left, miny = static_cast<double>(work.y) + i.top;
    const double x = std::round(std::clamp(c.x + c.width / 2.0 - w / 2.0, minx, minx + aw - w));
    const double y = std::round(std::clamp(c.y + c.height / 2.0 - h / 2.0, miny, miny + ah - h));
    for (double v : {x, y, w, h})
      if (!std::isfinite(v) || v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) return {};
    if (x + w > std::numeric_limits<int>::max() || y + h > std::numeric_limits<int>::max()) return {};
    // ConfigureWindow serializes origins as signed 16-bit and extents as unsigned
    // 16-bit fields. Reject proposals instead of silently wrapping native requests.
    if (x < std::numeric_limits<std::int16_t>::min() || x > std::numeric_limits<std::int16_t>::max() ||
        y < std::numeric_limits<std::int16_t>::min() || y > std::numeric_limits<std::int16_t>::max() ||
        w > std::numeric_limits<std::uint16_t>::max() || h > std::numeric_limits<std::uint16_t>::max()) return {};
    return ClientRect{static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h)};
  }
  std::expected<void, core::AppError> transact(core::ReplicaSettings &accepted, const core::ReplicaSettings &requested, SettingsOperations ops) noexcept
  {
    if (requested.opacity_percent < 0 || requested.opacity_percent > 100 || !core::valid_wheel_zoom_percent(requested.wheel_zoom_percent) ||
        (requested.mode != core::OverlayMode::Editable && requested.mode != core::OverlayMode::PassThrough) ||
        (requested.border != core::BorderPreference::Bordered && requested.border != core::BorderPreference::Borderless) ||
        (requested.region && (!requested.region->width || !requested.region->height)) ||
        (requested.fps_cap != core::FpsCap::Fps30 && requested.fps_cap != core::FpsCap::Fps60 && requested.fps_cap != core::FpsCap::Fps144 && requested.fps_cap != core::FpsCap::Unlimited)) return std::unexpected(core::AppError{core::AppErrorCode::InvalidValue});
    bool captured = false;
    try {
      if (!ops.snapshot || !ops.mutate || !ops.restore) return std::unexpected(core::AppError{core::AppErrorCode::PlatformOperationFailure});
      if (auto snapshot = ops.snapshot(ops.context); !snapshot) return std::unexpected(snapshot.error());
      captured = true;
      if (auto r = ops.mutate(ops.context, requested); !r) {
        const bool restored = ops.restore(ops.context);
        return std::unexpected(restored ? r.error() : core::AppError{core::AppErrorCode::RuntimeStateLost, r.error().native_detail});
      }
      accepted = requested;
      return {};
    } catch (...) {
      bool restored = !captured;
      try {
        if (captured) restored = ops.restore(ops.context);
      } catch (...) {
      }
      return std::unexpected(core::AppError{restored ? core::AppErrorCode::PlatformOperationFailure : core::AppErrorCode::RuntimeStateLost});
    }
  }
  void SelectionMapping::begin() noexcept
  {
    active_ = true;
    arm_sequence_ = sequence_;
    dragging_ = terminal_ = false;
    preview_.reset();
    result_.reset();
  }
  void SelectionMapping::cancel() noexcept
  {
    if (active_) {
      active_ = dragging_ = false;
      terminal_ = true;
      preview_.reset();
      result_.reset();
    }
  }
  void SelectionMapping::publish(render::PresentationSnapshot s, core::Extent client) noexcept
  {
    if (!s.content.width || !s.content.height || !s.framebuffer.width || !s.framebuffer.height || !client.width || !client.height) {
      cancel();
      snapshot_.reset();
      client_ = {};
      sequence_ = 0;
      return;
    }
    if (s.present_sequence <= sequence_) return;
    // A content edge ends even an armed gesture. View-only changes before press
    // still belong to the requested full-view transition and must keep it armed.
    if (active_ && snapshot_ && (snapshot_->content != s.content || (dragging_ && (snapshot_->framebuffer != s.framebuffer || snapshot_->viewport != s.viewport || !s.selection_full_view || client_ != client)))) cancel();
    sequence_ = s.present_sequence;
    snapshot_ = s;
    client_ = client;
  }
  render::PixelPoint SelectionMapping::point(double x, double y) const noexcept { return {x * snapshot_->framebuffer.width / client_.width, y * snapshot_->framebuffer.height / client_.height}; }
  void SelectionMapping::press(double x, double y) noexcept
  {
    if (!active_ || !snapshot_ || !snapshot_->selection_full_view || sequence_ <= arm_sequence_ || !std::isfinite(x) || !std::isfinite(y)) return;
    auto p = point(x, y);
    const auto &v = snapshot_->viewport;
    if (p.x < v.x || p.y < v.y || p.x >= static_cast<double>(v.x) + v.width || p.y >= static_cast<double>(v.y) + v.height) return;
    start_ = p;
    dragging_ = true;
  }
  void SelectionMapping::move(double x, double y) noexcept
  {
    if (active_ && dragging_) preview_ = render::selection_region(snapshot_->content, snapshot_->viewport, start_, point(x, y));
  }
  void SelectionMapping::release(double x, double y) noexcept
  {
    if (!active_ || !dragging_) return;
    move(x, y);
    result_ = preview_;
    preview_.reset();
    active_ = dragging_ = false;
    terminal_ = true;
  }
}    // namespace avb::linux_platform::detail
