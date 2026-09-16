/// @file
/// @brief Bind preferred-language selection to the Windows user-language query.

#include "platform/windows/windows_ui_language.hpp"

#include "platform/windows/detail/ui_language_query.hpp"

namespace avb::platform::windows {

  i18n::Language windows_ui_language() noexcept
  {
    return detail::read_ui_language(&GetUserPreferredUILanguages);
  }

}    // namespace avb::platform::windows
