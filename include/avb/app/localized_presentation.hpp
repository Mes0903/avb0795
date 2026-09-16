/// @file
/// @brief Localized projection of portable facts without querying or mutating their owners.
/// Catalog-derived results borrow program-lifetime UTF-8 literals. The exclusion
/// selector instead borrows whichever caller-supplied message matches the status.
#ifndef AVB0795_APP_LOCALIZED_PRESENTATION_HPP
#define AVB0795_APP_LOCALIZED_PRESENTATION_HPP

#include "avb/core/types.hpp"
#include "avb/i18n/language.hpp"

#include <optional>
#include <string_view>

namespace avb::app {

  struct BridgeFeedback;

  /// @brief Map a portable error code to user-facing catalog text.
  /// Native numeric detail and diagnostic labels remain separate; unknown codes
  /// select the localized unknown-error message.
  [[nodiscard]] std::string_view
  error_text(core::AppErrorCode code, i18n::Language language) noexcept;
  /// @brief Describe visible lifecycle only, with Error as the unknown-enum fallback.
  /// This wording cannot establish whether a runtime is still draining resources.
  [[nodiscard]] std::string_view
  lifecycle_text(core::SessionLifecycle lifecycle,
                 i18n::Language language) noexcept;
  /// @brief Select the localized display/window category label for a source row.
  /// @pre kind is a supported source category.
  [[nodiscard]] std::string_view
  source_kind_text(core::SourceKind kind, i18n::Language language) noexcept;
  /// @brief Explain reported bridge feedback without predicting graphics capability.
  /// A concrete fallback reason takes precedence over mode; None selects the mode
  /// label and an unknown reason uses the generic CPU-copy label.
  [[nodiscard]] std::string_view
  bridge_text(BridgeFeedback feedback, i18n::Language language) noexcept;
  /// @brief Warn for detected unsupported HDR or an unknown color observation.
  /// SDR and unknown enum values return no warning; absence does not independently
  /// verify the source's color properties.
  [[nodiscard]] std::optional<std::string_view>
  color_warning(core::ColorSupport support, i18n::Language language) noexcept;
  /// @brief Select a caller-provided warning for unsupported/failed capture exclusion.
  /// NotAttempted, Excluded and unknown statuses return no warning. The selected
  /// string_view aliases its input; this helper does not extend message storage lifetime.
  [[nodiscard]] std::optional<std::string_view>
  exclusion_warning(core::CaptureExclusionStatus status,
                    std::string_view unsupported,
                    std::string_view failed) noexcept;

}    // namespace avb::app

#endif    // AVB0795_APP_LOCALIZED_PRESENTATION_HPP
