/// @file
/// @brief Application-owned language resolution and accepted preference persistence.
#include "avb/app/application.hpp"

#include "avb/i18n/language.hpp"

#include <chrono>

namespace avb::app {

  void Application::refresh_language() noexcept
  {
    // Query only for System. A manual request must remain independent of OS query
    // failures; an absent/throwing query supplies the same safe English fallback.
    auto system_language = i18n::Language::English;
    if (config_.language == i18n::LanguagePreference::System &&
        system_language_query_) {
      try {
        system_language = system_language_query_();
      } catch (...) {
        system_language = i18n::Language::English;
      }
    }
    effective_language_ =
      i18n::resolve_language(config_.language, system_language);
  }

  void Application::handle(const SetLanguage &command,
                           std::chrono::steady_clock::time_point)
  {
    if (i18n::language_preference_token(command.preference).empty()) {
      reject(core::AppError{core::AppErrorCode::InvalidValue});
      return;
    }
    // Re-selecting System intentionally queries again, even without a config
    // change. Persist the request rather than freezing the resulting language.
    const bool changed = config_.language != command.preference;
    config_.language = command.preference;
    refresh_language();
    config_dirty_ = config_dirty_ || changed;
    // An unchanged request can still retry an earlier failed whole-config save.
    // Save failure leaves this accepted language in effect for the next projection.
    if (config_dirty_) {
      persist_config();
    }
  }

}    // namespace avb::app
