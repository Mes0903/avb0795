/// @file
/// @brief ASCII family matching and exact persistence-token conversion without locale state.
#include "avb/i18n/language.hpp"

#include <array>
#include <cstddef>

namespace avb::i18n {

  std::optional<Language> match_language_tag(std::string_view tag) noexcept
  {
    const auto lower = [](char value) {
      return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value + ('a' - 'A'))
               : value;
    };
    const auto alnum = [](char value) {
      return (value >= 'a' && value <= 'z') ||
             (value >= 'A' && value <= 'Z') ||
             (value >= '0' && value <= '9');
    };

    // Validate the entire suffix before choosing a supported prefix: malformed
    // tags must not become an accidental match merely because they begin with en/zh/ja.
    if (tag.size() < 2 || (tag.size() > 2 && tag[2] != '-')) {
      return std::nullopt;
    }
    if (tag.size() > 2) {
      std::size_t length{};
      for (std::size_t index = 3; index < tag.size(); ++index) {
        if (tag[index] == '-') {
          if (length == 0) {
            return std::nullopt;
          }
          length = 0;
        }
        else if (!alnum(tag[index]) || ++length > 8) {
          return std::nullopt;
        }
      }
      if (length == 0) {
        return std::nullopt;
      }
    }

    // Region/script subtags do not select a fourth catalog. The chosen product
    // policy maps the whole Chinese family to its Traditional Chinese translation.
    const char first = lower(tag[0]);
    const char second = lower(tag[1]);
    if (first == 'e' && second == 'n') {
      return Language::English;
    }
    if (first == 'z' && second == 'h') {
      return Language::TraditionalChinese;
    }
    if (first == 'j' && second == 'a') {
      return Language::Japanese;
    }
    return std::nullopt;
  }

  std::optional<LanguagePreference>
  parse_language_preference(std::string_view token) noexcept
  {
    // Compare through the serializer so persistence has one canonical spelling
    // per request, independent of the more permissive OS tag-family matcher.
    constexpr std::array preferences{
      LanguagePreference::System,
      LanguagePreference::English,
      LanguagePreference::TraditionalChinese,
      LanguagePreference::Japanese,
    };
    for (const auto preference : preferences) {
      if (language_preference_token(preference) == token) {
        return preference;
      }
    }
    return std::nullopt;
  }

  std::string_view
  language_preference_token(LanguagePreference preference) noexcept
  {
    switch (preference) {
    case LanguagePreference::System:
      return "system";
    case LanguagePreference::English:
      return "en";
    case LanguagePreference::TraditionalChinese:
      return "zh-TW";
    case LanguagePreference::Japanese:
      return "ja";
    }
    return {};
  }

  Language resolve_language(LanguagePreference preference,
                            Language system_language) noexcept
  {
    switch (preference) {
    case LanguagePreference::System:
      switch (system_language) {
      case Language::English:
      case Language::TraditionalChinese:
      case Language::Japanese:
        return system_language;
      }
      break;
    case LanguagePreference::English:
      return Language::English;
    case LanguagePreference::TraditionalChinese:
      return Language::TraditionalChinese;
    case LanguagePreference::Japanese:
      return Language::Japanese;
    }

    // Persisted or platform-provided enum values may be invalid.
    return Language::English;
  }

}    // namespace avb::i18n
