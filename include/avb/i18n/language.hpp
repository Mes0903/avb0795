/// @file
/// @brief Pure language policy separating persisted requests from effective catalog choices.
#ifndef AVB0795_I18N_LANGUAGE_HPP
#define AVB0795_I18N_LANGUAGE_HPP

#include <optional>
#include <string_view>

namespace avb::i18n {

  /// @brief A supported catalog language after system policy has been resolved.
  /// This enum carries no locale object, OS query or process-wide mutable setting.
  enum class Language { English,
                        TraditionalChinese,
                        Japanese };

  /// @brief The user's persisted request, which may continue to follow system policy.
  /// Keep System as the request even when the effective language is known; otherwise
  /// saving preferences would silently turn a following policy into a manual choice.
  enum class LanguagePreference { System,
                                  English,
                                  TraditionalChinese,
                                  Japanese };

  /// @brief Match an ASCII language family from a borrowed system-language tag.
  /// Recognize en, zh and ja case-insensitively, optionally followed by hyphenated
  /// subtags of one to eight ASCII alphanumeric characters. This deliberately small
  /// grammar does not validate full language-tag semantics or interpret script/region:
  /// every accepted zh tag maps to the Traditional Chinese catalog.
  /// @return No value for malformed or unsupported tags; the caller chooses list
  /// precedence and fallback. No input storage is retained or normalized in place.
  [[nodiscard]] std::optional<Language>
  match_language_tag(std::string_view tag) noexcept;
  /// @brief Parse only the exact persistence tokens system, en, zh-TW and ja.
  /// Matching is case-sensitive and does not trim whitespace or accept family aliases.
  /// Invalid input returns no value so the persistence layer can choose its recovery policy.
  [[nodiscard]] std::optional<LanguagePreference>
  parse_language_preference(std::string_view token) noexcept;
  /// @brief Borrow the canonical program-lifetime token for a requested preference.
  /// System remains system without querying the environment. Invalid enum values
  /// return an empty view, allowing command/save validation before side effects.
  [[nodiscard]] std::string_view
  language_preference_token(LanguagePreference preference) noexcept;
  /// @brief Resolve a request against an already-queried system catalog language.
  /// Manual preferences ignore system_language. System uses a supported supplied
  /// language; invalid preference or relevant system values fall back to English.
  /// The caller retains the original preference for persistence and presentation.
  [[nodiscard]] Language
  resolve_language(LanguagePreference preference,
                   Language system_language) noexcept;

}    // namespace avb::i18n

#endif    // AVB0795_I18N_LANGUAGE_HPP
