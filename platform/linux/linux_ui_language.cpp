/// @file
/// @brief Linux-only locale spelling and environment precedence adaptation.
#include "platform/linux/linux_ui_language.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

namespace avb::linux_platform {
  namespace {
    std::string normalize(std::string_view locale)
    {
      std::string tag{locale.substr(0, locale.find_first_of(".@"))};
      std::replace(tag.begin(), tag.end(), '_', '-');
      return tag;
    }
  }    // namespace
  i18n::Language linux_ui_language()
  {
    std::string effective{"C"};
    for (const auto *key : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
      if (const auto *value = std::getenv(key); value && *value) {
        effective = value;
        break;
      }
    }
    const auto normalized = normalize(effective);
    if (normalized == "C" || normalized == "POSIX") return i18n::Language::English;
    if (const auto *value = std::getenv("LANGUAGE")) {
      std::string_view preferences{value};
      while (!preferences.empty()) {
        const auto separator = preferences.find(':');
        if (const auto language = i18n::match_language_tag(normalize(preferences.substr(0, separator)))) return *language;
        if (separator == std::string_view::npos) break;
        preferences.remove_prefix(separator + 1);
      }
    }
    return i18n::match_language_tag(normalized).value_or(i18n::Language::English);
  }
}    // namespace avb::linux_platform
