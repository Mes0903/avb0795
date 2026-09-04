#ifndef AIRELINNA_PLATFORM_WINDOWS_WINDOWS_SOURCE_CATALOG_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WINDOWS_SOURCE_CATALOG_HPP

#include "airelinna/app/ports.hpp"
#include "platform/windows/windows_source_registry.hpp"

// DXGI's shared declarations rely on Win32 scalar types being defined first.
// clang-format off
#include <windows.h>
#include <dxgicommon.h>
// clang-format on

#include <optional>

namespace airelinna::platform::windows {

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

[[nodiscard]] bool
is_window_source_eligible(const WindowEligibilityFacts &facts) noexcept;

[[nodiscard]] core::ColorSupport classify_color_support(
    std::optional<DXGI_COLOR_SPACE_TYPE> color_space) noexcept;

class WindowsSourceCatalog final : public app::ISourceCatalog {
public:
  explicit WindowsSourceCatalog(WindowsSourceRegistry &registry) noexcept;

  [[nodiscard]]
  std::expected<std::vector<core::SourceDescriptor>, core::AppError>
  enumerate() override;

private:
  WindowsSourceRegistry &registry_;
};

} // namespace airelinna::platform::windows

#endif // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_SOURCE_CATALOG_HPP
