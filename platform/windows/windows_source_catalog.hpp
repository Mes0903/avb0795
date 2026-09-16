/// @file
/// @brief Native candidate filtering and descriptor publication with stable item identity.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_SOURCE_CATALOG_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_SOURCE_CATALOG_HPP

#include "avb/app/ports.hpp"
#include "platform/windows/windows_source_registry.hpp"

// DXGI's shared declarations rely on Win32 scalar types being defined first.
// clang-format off
#include <windows.h>
#include <dxgicommon.h>
// clang-format on

#include <optional>

namespace avb::platform::windows {

  /// Native enumeration facts reduced to values for deterministic filtering.
  struct WindowEligibilityFacts {
    bool visible{};
    bool cloak_query_succeeded{};
    bool cloaked{};
    bool has_nonzero_area{};
    bool tool_window{};
    bool tool_window_intended_for_selection{};
    bool owned_by_current_process{};
    bool has_usable_title{};
  };

  /// Exclude hidden, cloaked, unusable, or application-owned window candidates.
  [[nodiscard]] bool
  is_window_source_eligible(const WindowEligibilityFacts &facts) noexcept;

  /// Translate a per-refresh DXGI snapshot; unavailable information stays Unknown.
  [[nodiscard]] core::ColorSupport classify_color_support(
    std::optional<DXGI_COLOR_SPACE_TYPE> color_space) noexcept;

  /// Main-thread discovery adapter borrowing the process-owned source registry.
  /// Each descriptor is published only after its item's Closed watcher exists.
  class WindowsSourceCatalog final : public app::ISourceCatalog {
  public:
    explicit WindowsSourceCatalog(WindowsSourceRegistry &registry) noexcept;

    /// Replace the snapshot transactionally with eligible window/display sources.
    /// Color support is best effort and does not create continuous monitoring.
    [[nodiscard]]
    std::expected<std::vector<core::SourceDescriptor>, core::AppError>
    enumerate() override;

  private:
    WindowsSourceRegistry &registry_;
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_SOURCE_CATALOG_HPP
