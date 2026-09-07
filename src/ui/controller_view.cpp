/// @file ImGui widget projection and value-command emission on the host's main thread.
#include "airelinna/ui/controller_view.hpp"

#include "airelinna/core/hotkey.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace airelinna::ui {
  namespace {

    // Treat external labels as literal bytes, never as format strings.
    void text(std::string_view value)
    {
      ImGui::TextUnformatted(value.data(), value.data() + value.size());
    }

    void diagnostic(std::string_view label,
                    const std::optional<std::string_view> &value)
    {
      if (!value) {
        return;
      }
      ImGui::Text("%.*s: %.*s", static_cast<int>(label.size()), label.data(),
                  static_cast<int>(value->size()), value->data());
    }

    void disabled_explanation(const app::ControlState &control)
    {
      if (control.enabled || control.explanation.empty()) {
        return;
      }
      ImGui::SameLine();
      ImGui::TextDisabled("%.*s", static_cast<int>(control.explanation.size()),
                          control.explanation.data());
    }

    [[nodiscard]] bool button(const char *label,
                              const app::ControlState &control)
    {
      ImGui::BeginDisabled(!control.enabled);
      const bool pressed = ImGui::Button(label);
      ImGui::EndDisabled();
      disabled_explanation(control);
      return pressed;
    }

    [[nodiscard]] bool radio_button(const char *label, bool active, const app::ControlState &control)
    {
      ImGui::BeginDisabled(!control.enabled);
      const bool pressed = ImGui::RadioButton(label, active);
      ImGui::EndDisabled();
      disabled_explanation(control);
      return pressed;
    }

    // The sink owns delivery policy. Widgets retain neither commands nor a
    // reference to the use-case orchestrator and never call platform/runtime operations.
    template <typename Command>
    void emit_if(bool requested, const CommandSink &sink, Command command)
    {
      if (requested && sink) {
        sink(app::AppCommand{std::move(command)});
      }
    }

  }    // namespace

  void ControllerView::render(const app::ControllerViewState &state,
                              const CommandSink &command_sink)
  {
    // Begin must be paired with End even when the window is collapsed.
    if (!ImGui::Begin("Airelinna Controller")) {
      ImGui::End();
      return;
    }

    ImGui::Text("Status: %.*s", static_cast<int>(state.status_text.size()),
                state.status_text.data());
    diagnostic("Session error", state.session_error_text);
    diagnostic("Notice", state.application_notice_text);
    diagnostic("Color", state.color_warning);
    diagnostic("Controller exclusion", state.controller_exclusion_warning);
    diagnostic("Overlay exclusion", state.overlay_exclusion_warning);
    diagnostic("Cleanup", state.drain_warning);
    if (state.bridge) {
      ImGui::Text("Bridge: %.*s",
                  static_cast<int>(state.bridge->status_text.size()),
                  state.bridge->status_text.data());
    }

    ImGui::SeparatorText("Sources");
    emit_if(button("Refresh", state.controls.refresh), command_sink,
            app::RefreshSources{});
    for (const auto &source : state.sources) {
      ImGui::PushID(source.id.value.c_str());
      text(source.kind_glyph);
      ImGui::SameLine();
      ImGui::BeginDisabled(!source.select.enabled);
      // External titles are text, not ImGui labels: keep the interaction ID
      // separate so literal "##" / "###" never hide part of a window name.
      const auto label_position = ImGui::GetCursorPos();
      const auto label_size = ImGui::CalcTextSize(source.label.c_str(), nullptr, false);
      const bool selected =
        ImGui::Selectable("##source", source.selected,
                          ImGuiSelectableFlags_None, {0.0F, label_size.y});
      ImGui::SetCursorPos(label_position);
      text(source.label);
      ImGui::EndDisabled();
      disabled_explanation(source.select);
      emit_if(selected, command_sink,
              app::SelectSource{state.session_id, source.id});
      ImGui::PopID();
    }

    // Every session action uses this render's projected generation token;
    // The use-case orchestrator remains responsible for rejecting stale-view commands.
    emit_if(button("Start", state.controls.start), command_sink,
            app::Start{state.session_id});
    emit_if(button("Stop", state.controls.stop), command_sink,
            app::Stop{state.session_id});

    ImGui::SeparatorText("Mode");
    emit_if(radio_button("Editable",
                         state.settings.mode == core::OverlayMode::Editable,
                         state.controls.editable),
            command_sink,
            app::SetMode{state.session_id, core::OverlayMode::Editable});
    emit_if(radio_button(
              "Pass-through",
              state.settings.mode == core::OverlayMode::PassThrough,
              state.controls.pass_through),
            command_sink,
            app::SetMode{state.session_id, core::OverlayMode::PassThrough});

    ImGui::SeparatorText("Frame rate");
    emit_if(radio_button("30 FPS",
                         state.settings.fps_cap == core::FpsCap::Fps30,
                         state.controls.fps),
            command_sink,
            app::SetFps{state.session_id, core::FpsCap::Fps30});
    emit_if(radio_button("60 FPS",
                         state.settings.fps_cap == core::FpsCap::Fps60,
                         state.controls.fps),
            command_sink,
            app::SetFps{state.session_id, core::FpsCap::Fps60});
    emit_if(radio_button("144 FPS",
                         state.settings.fps_cap == core::FpsCap::Fps144,
                         state.controls.fps),
            command_sink,
            app::SetFps{state.session_id, core::FpsCap::Fps144});
    emit_if(radio_button("Unlimited",
                         state.settings.fps_cap == core::FpsCap::Unlimited,
                         state.controls.fps),
            command_sink,
            app::SetFps{state.session_id, core::FpsCap::Unlimited});

    ImGui::SeparatorText("Border");
    emit_if(radio_button(
              "Bordered",
              state.settings.border == core::BorderPreference::Bordered,
              state.controls.bordered),
            command_sink,
            app::SetBorder{state.session_id,
                           core::BorderPreference::Bordered});
    emit_if(radio_button(
              "Borderless",
              state.settings.border == core::BorderPreference::Borderless,
              state.controls.borderless),
            command_sink,
            app::SetBorder{state.session_id,
                           core::BorderPreference::Borderless});

    ImGui::SeparatorText("Extensions");
    // Edit a fresh value snapshot each frame; only an accepted command updates
    // the next projected checkbox state.
    auto extensions = state.settings.extensions;
    const auto &extension_control = state.controls.browser_capture_keep_alive;
    ImGui::BeginDisabled(!extension_control.enabled);
    const bool extension_changed = ImGui::Checkbox(
      "Browser capture keep-alive (experimental)",
      &extensions.browser_capture_keep_alive);
    ImGui::EndDisabled();
    // Keep the help marker outside the disabled block so it remains hoverable.
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort |
                             ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::BeginTooltip();
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0F);
      ImGui::TextUnformatted(
        "Attempts to keep covered browser windows rendering during window capture. "
        "May also affect other browser windows and increase CPU/GPU usage and power "
        "consumption. Does not change browser settings. Minimized windows, inactive "
        "tabs, protected video, and locked or sleeping displays are not guaranteed. "
        "Browser recovery after disabling is best-effort. Experimental: browser "
        "updates may break this workaround.");
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
    disabled_explanation(extension_control);
    text(state.extensions_scope_text);
    emit_if(extension_changed, command_sink,
            app::SetExtensions{state.session_id, extensions});

    ImGui::SeparatorText("Hotkey");
    // Preserve an unfinished edit across app snapshots. Its text is not proof
    // that the OS accepted a binding; the separate confirmed flag is authoritative.
    if (!hotkey_edit_text_) {
      hotkey_edit_text_ = state.hotkey_display_text;
    }
    constexpr std::size_t hotkey_capacity{64};
    std::array<char, hotkey_capacity> hotkey_buffer{};
    const auto hotkey_length =
      (std::min) (hotkey_edit_text_->size(), hotkey_buffer.size() - 1);
    std::copy_n(hotkey_edit_text_->data(), hotkey_length,
                hotkey_buffer.data());
    ImGui::BeginDisabled(!state.controls.replace_hotkey.enabled);
    if (ImGui::InputText("Desired hotkey", hotkey_buffer.data(),
                         hotkey_buffer.size())) {
      hotkey_edit_text_ = hotkey_buffer.data();
    }
    ImGui::EndDisabled();
    disabled_explanation(state.controls.replace_hotkey);

    // Local syntax validation provides immediate feedback; OS conflict/rollback
    // checks occur only when the orchestrator processes the typed replacement command.
    const auto parsed_hotkey = core::parse_hotkey_chord(*hotkey_edit_text_);
    auto replace_control = state.controls.replace_hotkey;
    if (replace_control.enabled && !parsed_hotkey) {
      replace_control = {
        .enabled = false,
        .explanation = "Enter a valid hotkey chord.",
      };
    }
    const bool replace_requested = button("Replace hotkey", replace_control);
    if (replace_requested && parsed_hotkey && command_sink) {
      command_sink(app::AppCommand{
        app::ReplaceHotkey{state.session_id, *parsed_hotkey}});
      hotkey_edit_text_.reset();
    }
    ImGui::Text("Binding: %s",
                state.hotkey_binding_confirmed ? "confirmed" : "unconfirmed");

    emit_if(button("Quit", {.enabled = true}), command_sink, app::Quit{});
    ImGui::End();
  }

}    // namespace airelinna::ui
