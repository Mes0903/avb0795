/// @file
/// @brief Stable-identity language controls displaying requested and effective choices.
#include "language_selector.hpp"

#include "avb/i18n/catalog.hpp"

#include <imgui.h>

#include <string>

namespace avb::ui {
  namespace {

    [[nodiscard]] i18n::TextId
    preference_caption_id(i18n::LanguagePreference preference) noexcept
    {
      switch (preference) {
      case i18n::LanguagePreference::System:
        return i18n::TextId::FollowSystem;
      case i18n::LanguagePreference::English:
        return i18n::TextId::LanguageEnglish;
      case i18n::LanguagePreference::TraditionalChinese:
        return i18n::TextId::LanguageTraditionalChinese;
      case i18n::LanguagePreference::Japanese:
        return i18n::TextId::LanguageJapanese;
      }
      return i18n::TextId::FollowSystem;
    }

    [[nodiscard]] i18n::TextId
    language_caption_id(i18n::Language language) noexcept
    {
      switch (language) {
      case i18n::Language::English:
        return i18n::TextId::LanguageEnglish;
      case i18n::Language::TraditionalChinese:
        return i18n::TextId::LanguageTraditionalChinese;
      case i18n::Language::Japanese:
        return i18n::TextId::LanguageJapanese;
      }
      return i18n::TextId::LanguageEnglish;
    }

  }    // namespace

  void render_language_selector(const app::ControllerViewState &state,
                                const CommandSink &command_sink)
  {
    const auto language = state.effective_language;
    const auto &control = state.controls.language;
    ImGui::SeparatorText(i18n::text(i18n::TextId::GeneralSettings, language).data());

    // Translate the caption while preserving the combo identity across changes
    // in effective language, including a fresh resolution of the same System request.
    const auto combo_label =
      std::string{i18n::text(i18n::TextId::LanguageLabel, language)} +
      "###language";
    const auto preview = i18n::text(
      preference_caption_id(state.language_preference), language);

    ImGui::BeginDisabled(!control.enabled);
    if (ImGui::BeginCombo(combo_label.c_str(), preview.data())) {
      constexpr i18n::LanguagePreference preferences[]{
        i18n::LanguagePreference::System,
        i18n::LanguagePreference::English,
        i18n::LanguagePreference::TraditionalChinese,
        i18n::LanguagePreference::Japanese,
      };
      for (const auto preference : preferences) {
        // Enum identity plus a fixed suffix keeps each option distinct even when
        // localized/endonym captions coincide across languages.
        ImGui::PushID(static_cast<int>(preference));
        const auto caption =
          i18n::text(preference_caption_id(preference), language);
        const auto option_label =
          std::string{caption} + "###language_option";
        const bool selected = preference == state.language_preference;
        // Do not filter out the selected option: System can be reselected to
        // ask the owner to query its environment again without changing the request.
        if (ImGui::Selectable(option_label.c_str(), selected) && command_sink) {
          command_sink(app::AppCommand{app::SetLanguage{preference}});
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    if (state.language_preference == i18n::LanguagePreference::System) {
      ImGui::Text(
        "%s: %s",
        i18n::text(i18n::TextId::EffectiveLanguage, language).data(),
        i18n::text(language_caption_id(language), language).data());
    }
    if (!control.enabled && !control.explanation.empty()) {
      ImGui::TextWrapped("%.*s", static_cast<int>(control.explanation.size()),
                         control.explanation.data());
    }
  }

}    // namespace avb::ui
