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

  /// @brief Publish selectable descriptions backed by observed native identities.
  ///
  /// This main-thread adapter borrows a registry that must outlive it. Each
  /// descriptor receives an opaque registry ID only after the item's close
  /// watcher exists. A title or class-name fallback is for display and filtering,
  /// never a key for resolving the selected source later.
  class WindowsSourceCatalog final : public app::ISourceCatalog {
  public:
    explicit WindowsSourceCatalog(WindowsSourceRegistry &registry) noexcept;

    /// @brief Commit a newly enumerated set of eligible window and display sources.
    ///
    /// Active runtime leases prevent refresh. Unusable individual candidates are
    /// omitted; enumeration failure leaves the refresh uncommitted. Extent and
    /// color information describe this refresh only and do not establish ongoing
    /// availability, HDR support, or capture progress for minimized sources.
    [[nodiscard]]
    std::expected<std::vector<core::SourceDescriptor>, core::AppError>
    enumerate() override;

  private:
    WindowsSourceRegistry &registry_;
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_SOURCE_CATALOG_HPP
