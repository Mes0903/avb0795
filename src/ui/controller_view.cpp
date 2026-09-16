/// @file
/// @brief ImGui widget projection and value-command emission on the host's main thread.
#include "avb/ui/controller_view.hpp"

#include "avb/core/hotkey.hpp"
#include "avb/i18n/catalog.hpp"
#include "language_selector.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace avb::ui {
  namespace {

    // Treat external labels as literal bytes, never as format strings.
    void literal_text(std::string_view value)
    {
      ImGui::TextUnformatted(value.data(), value.data() + value.size());
    }

    /// Translate the domain's three contiguous key ranges at the UI boundary.
    /// Unsupported keys remain available for modal navigation and cancellation.
    [[nodiscard]] std::optional<core::HotkeyKey> portable_key(ImGuiKey key) noexcept
    {
      if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        return static_cast<core::HotkeyKey>(
          static_cast<int>(core::HotkeyKey::A) + key - ImGuiKey_A);
      }
      if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
        return static_cast<core::HotkeyKey>(
          static_cast<int>(core::HotkeyKey::Digit0) + key - ImGuiKey_0);
      }
      if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) {
        return static_cast<core::HotkeyKey>(
          static_cast<int>(core::HotkeyKey::F1) + key - ImGuiKey_F1);
      }
      return std::nullopt;
    }

    [[nodiscard]] bool recording_inputs_released()
    {
      for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key))) {
          return false;
        }
      }
      return true;
    }

    void diagnostic(std::string_view label, std::string_view value)
    {
      ImGui::TextWrapped("%.*s: %.*s", static_cast<int>(label.size()),
                         label.data(), static_cast<int>(value.size()),
                         value.data());
    }

    void diagnostic(
      std::string_view label,
      const std::optional<std::string_view> &value)
    {
      if (!value) {
        return;
      }
      diagnostic(label, *value);
    }

    // ImGui hashes only the suffix after ###, so changing localized captions
    // preserves focus, editing state and saved window identity across languages.
    [[nodiscard]] std::string
    widget_label(i18n::TextId id, const char *identity, i18n::Language language)
    {
      return std::string{i18n::text(id, language)} + "###" + identity;
    }

    void disabled_explanation(const app::ControlState &control)
    {
      if (control.enabled || control.explanation.empty()) {
        return;
      }
      ImGui::PushTextWrapPos(0.0F);
      ImGui::TextDisabled("%.*s", static_cast<int>(control.explanation.size()),
                          control.explanation.data());
      ImGui::PopTextWrapPos();
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

    enum class TitleGlyph { Minimize, Close };

    /// @brief Draw a title-integrated glyph while Button owns its input behavior.
    /// A transparent idle fill and zero border expose the shared title background;
    /// hover, held and keyboard-navigation feedback retain the caller's theme.
    /// Scoped style changes preserve content widgets. Exactly one item retains
    /// the stable ID, hit rectangle and last-item state used by tooltips and drag
    /// exclusion. Font-sized vector strokes keep the icons consistent across
    /// localized captions; the caller routes the returned action through its port.
    [[nodiscard]] bool title_button(const char *id, TitleGlyph glyph, float size)
    {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
      const bool pressed = ImGui::Button(id, {size, size});
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();

      const auto minimum = ImGui::GetItemRectMin();
      const auto maximum = ImGui::GetItemRectMax();
      const ImVec2 center{(minimum.x + maximum.x) * 0.5F,
                          (minimum.y + maximum.y) * 0.5F};
      const float extent = std::max(1.0F, ImGui::GetFontSize() * 0.35F);
      const float thickness = std::max(1.0F, ImGui::GetFontSize() / 18.0F);
      const auto color = ImGui::GetColorU32(ImGuiCol_Text);
      auto *const draw_list = ImGui::GetWindowDrawList();
      if (glyph == TitleGlyph::Minimize) {
        draw_list->AddLine({center.x - extent, center.y},
                           {center.x + extent, center.y}, color, thickness);
      } else {
        draw_list->AddLine({center.x - extent, center.y - extent},
                           {center.x + extent, center.y + extent}, color, thickness);
        draw_list->AddLine({center.x - extent, center.y + extent},
                           {center.x + extent, center.y - extent}, color, thickness);
      }
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

  ControllerFrame ControllerView::render(const app::ControllerViewState &state,
                                          const CommandSink &command_sink,
                                          const float resize_margin)
  {
    ControllerFrame frame;
    if (hotkey_recording_ &&
        (ImGui::GetIO().AppFocusLost ||
         hotkey_recording_->session_id != state.session_id ||
         !state.controls.replace_hotkey.enabled)) {
      cancel_hotkey_recording();
    }
    const auto language = state.effective_language;
    const auto window_label = widget_label(
      i18n::TextId::ControllerTitle, "avb0795 Controller", language);
    const auto *const viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    const float margin = std::isfinite(resize_margin)
                           ? std::max(8.0F, resize_margin) : 8.0F;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{margin, margin});
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoScrollWithMouse;
    const bool visible = ImGui::Begin(window_label.c_str(), nullptr, flags);
    ImGui::PopStyleVar(3);
    if (!visible) {
      ImGui::End();
      return frame;
    }

    // Draw text in a clipped title region; right-side controls keep their hit
    // boxes even when the caption is long or the native window is narrow.
    const auto origin = ImGui::GetWindowPos();
    const auto row = ImGui::GetCursorScreenPos();
    const auto &style = ImGui::GetStyle();
    const float button_width = ImGui::GetFrameHeight();
    const float controls_left = std::max(row.x,
      origin.x + ImGui::GetWindowSize().x - margin -
        2.0F * button_width - style.ItemSpacing.x);
    const auto title = i18n::text(i18n::TextId::ControllerTitle, language);
    auto *const draw_list = ImGui::GetWindowDrawList();
    draw_list->PushClipRect(row,
      {std::max(row.x, controls_left - style.ItemSpacing.x), row.y + button_width},
      true);
    draw_list->AddText({row.x, row.y + style.FramePadding.y},
      ImGui::GetColorU32(ImGuiCol_Text), title.data(), title.data() + title.size());
    draw_list->PopClipRect();
    ImGui::SetCursorScreenPos({controls_left, row.y});
    frame.minimize_requested = title_button("###controller_minimize",
                                            TitleGlyph::Minimize, button_width);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", i18n::text(i18n::TextId::Minimize, language).data());
    }
    ImGui::SameLine();
    emit_if(title_button("###controller_close", TitleGlyph::Close, button_width),
            command_sink, app::Quit{});
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", i18n::text(i18n::TextId::Quit, language).data());
    }
    frame.drag_right = std::max(0.0F, controls_left - origin.x);
    frame.drag_bottom = row.y + button_width + style.ItemSpacing.y - origin.y;
    ImGui::Separator();

    // A dedicated scrolling child keeps the title and its native hit geometry
    // fixed while preserving ImGui ownership of content input and popup widgets.
    if (ImGui::BeginChild("controller_content", {0.0F, 0.0F})) {
      render_contents(state, command_sink);
    }
    ImGui::EndChild();
    // Popup IDs belong to the fixed root, independent of the child's scrolling.
    render_hotkey_recorder(state, command_sink);
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                    ImGuiPopupFlags_AnyPopupLevel) ||
        (ImGui::IsAnyItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
      frame.drag_right = 0.0F;
      frame.drag_bottom = 0.0F;
    }
    ImGui::End();
    return frame;
  }

  void ControllerView::render_contents(const app::ControllerViewState &state,
                                       const CommandSink &command_sink)
  {
    const auto language = state.effective_language;
    render_language_selector(state, command_sink);

    diagnostic(i18n::text(i18n::TextId::StatusLabel, language),
               state.status_text);
    diagnostic(i18n::text(i18n::TextId::SessionErrorLabel, language),
               state.session_error_text);
    diagnostic(i18n::text(i18n::TextId::NoticeLabel, language),
               state.application_notice_text);
    diagnostic(i18n::text(i18n::TextId::ColorLabel, language),
               state.color_warning);
    diagnostic(i18n::text(i18n::TextId::ControllerExclusionLabel, language),
               state.controller_exclusion_warning);
    diagnostic(i18n::text(i18n::TextId::OverlayExclusionLabel, language),
               state.overlay_exclusion_warning);
    diagnostic(i18n::text(i18n::TextId::CleanupLabel, language),
               state.drain_warning);
    if (state.bridge) {
      diagnostic(i18n::text(i18n::TextId::BridgeLabel, language),
                 state.bridge->status_text);
    }

    ImGui::SeparatorText(i18n::text(i18n::TextId::Sources, language).data());
    emit_if(button(widget_label(i18n::TextId::Refresh, "refresh", language)
                     .c_str(),
                   state.controls.refresh),
            command_sink,
            app::RefreshSources{});
    auto auto_refresh = state.auto_refresh_enabled;
    ImGui::BeginDisabled(!state.controls.auto_refresh.enabled);
    const bool refresh_changed = ImGui::Checkbox(
      widget_label(i18n::TextId::AutoRefresh, "auto_refresh", language).c_str(),
      &auto_refresh);
    ImGui::EndDisabled();
    emit_if(refresh_changed, command_sink, app::SetAutoRefresh{auto_refresh});
    if (state.auto_refresh_paused) {
      ImGui::TextDisabled("%s",
        i18n::text(i18n::TextId::AutoRefreshPaused, language).data());
    }
    // A full-row selectable owns identity and navigation, while literal text
    // occupies independent columns. Native diagnostics remain presentation values;
    // the emitted command retains the opaque catalog ID and session generation.
    constexpr auto table_flags = ImGuiTableFlags_RowBg |
      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersH;
    if (ImGui::BeginTable("sources_table", 3, table_flags)) {
      ImGui::TableSetupColumn(i18n::text(i18n::TextId::SourceType, language).data(),
                              ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn(i18n::text(i18n::TextId::SourceName, language).data(),
                              ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("HWND", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();
      for (const auto &source : state.sources) {
        ImGui::PushID(source.id.value.c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginDisabled(!source.select.enabled);
        const auto row_position = ImGui::GetCursorPos();
        const bool selected = ImGui::Selectable(
          "##source", source.selected, ImGuiSelectableFlags_SpanAllColumns,
          {0.0F, ImGui::GetTextLineHeight()});
        const bool hovered = ImGui::IsItemHovered(
          ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled);
        ImGui::SetCursorPos(row_position);
        const auto disabled = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        const auto type_color = source.kind == core::SourceKind::Window
          ? ImVec4{1.0F, 0.45F, 0.7F, 1.0F}
          : ImVec4{0.65F, 0.6F, 1.0F, 1.0F};
        ImGui::PushStyleColor(ImGuiCol_Text,
          source.select.enabled ? type_color : disabled);
        literal_text(source.kind_glyph);
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, source.select.enabled
          ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : disabled);
        literal_text(source.label);
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text,
          source.select.enabled && !source.native_handle_text.empty()
          ? ImVec4{0.4F, 0.9F, 0.55F, 1.0F} : disabled);
        literal_text(source.native_handle_text.empty()
          ? std::string_view{"—"} : source.native_handle_text);
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (hovered) {
          ImGui::BeginTooltip();
          ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0F);
          literal_text(source.label);
          ImGui::PopTextWrapPos();
          ImGui::EndTooltip();
        }
        emit_if(selected, command_sink,
                app::SelectSource{state.session_id, source.id});
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    // Every session action uses this render's projected generation token;
    // The use-case orchestrator remains responsible for rejecting stale-view commands.
    emit_if(button(widget_label(i18n::TextId::Start, "start", language).c_str(),
                   state.controls.start),
            command_sink,
            app::Start{state.session_id});
    emit_if(button(widget_label(i18n::TextId::Stop, "stop", language).c_str(),
                   state.controls.stop),
            command_sink,
            app::Stop{state.session_id});

    ImGui::SeparatorText(i18n::text(i18n::TextId::Mode, language).data());
    emit_if(radio_button(widget_label(i18n::TextId::Editable, "editable", language)
                           .c_str(),
                         state.settings.mode == core::OverlayMode::Editable,
                         state.controls.editable),
            command_sink,
            app::SetMode{state.session_id, core::OverlayMode::Editable});
    emit_if(radio_button(
              widget_label(i18n::TextId::PassThrough, "pass_through", language)
                .c_str(),
              state.settings.mode == core::OverlayMode::PassThrough,
              state.controls.pass_through),
            command_sink,
            app::SetMode{state.session_id, core::OverlayMode::PassThrough});

    // A draft belongs to the session that started the gesture. A newer snapshot
    // cannot redirect an active edit; the orchestrator rejects stale commands.
    int opacity = opacity_editing_ ? opacity_editing_->percent
                                    : state.settings.opacity_percent;
    ImGui::BeginDisabled(!state.controls.opacity.enabled);
    const bool opacity_changed = ImGui::DragInt(
      widget_label(i18n::TextId::Opacity, "opacity", language).c_str(),
      &opacity, 1.0F, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
    const bool opacity_active = ImGui::IsItemActive();
    const bool opacity_finished = ImGui::IsItemDeactivatedAfterEdit();
    if (!opacity_editing_ && (ImGui::IsItemActivated() || opacity_changed)) {
      opacity_editing_ = OpacityEditing{.session_id = state.session_id,
                                       .percent = opacity};
    }
    ImGui::EndDisabled();
    if (opacity_editing_) {
      opacity_editing_->percent = opacity;
      emit_if(opacity_changed, command_sink,
              app::SetOpacity{opacity_editing_->session_id, opacity, false});
      // Queue the last preview first. Commit only flushes the accepted value,
      // so a rejected preview is never implicitly retried when the gesture ends.
      emit_if(opacity_finished, command_sink,
              app::SetOpacity{opacity_editing_->session_id, opacity, true});
      if (!opacity_active) {
        opacity_editing_.reset();
      }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort |
                             ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::BeginTooltip();
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0F);
      literal_text(i18n::text(i18n::TextId::OpacityHelp, language));
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
    disabled_explanation(state.controls.opacity);

    render_region(state, command_sink);

    ImGui::SeparatorText(
      i18n::text(i18n::TextId::FrameRate, language).data());
    emit_if(radio_button(widget_label(i18n::TextId::Fps30, "fps_30", language)
                           .c_str(),
                         state.settings.fps_cap == core::FpsCap::Fps30,
                         state.controls.fps),
            command_sink,
            app::SetFps{state.session_id, core::FpsCap::Fps30});
    emit_if(radio_button(widget_label(i18n::TextId::Fps60, "fps_60", language)
                           .c_str(),
                         state.settings.fps_cap == core::FpsCap::Fps60,
                         state.controls.fps),
            command_sink,
            app::SetFps{state.session_id, core::FpsCap::Fps60});
    emit_if(radio_button(widget_label(i18n::TextId::Fps144, "fps_144", language)
                           .c_str(),
                         state.settings.fps_cap == core::FpsCap::Fps144,
                         state.controls.fps),
            command_sink,
            app::SetFps{state.session_id, core::FpsCap::Fps144});
    emit_if(radio_button(widget_label(i18n::TextId::Unlimited, "fps_unlimited",
                                      language)
                           .c_str(),
                         state.settings.fps_cap == core::FpsCap::Unlimited,
                         state.controls.fps),
            command_sink,
            app::SetFps{state.session_id, core::FpsCap::Unlimited});

    ImGui::SeparatorText(i18n::text(i18n::TextId::Border, language).data());
    emit_if(radio_button(
              widget_label(i18n::TextId::Bordered, "bordered", language)
                .c_str(),
              state.settings.border == core::BorderPreference::Bordered,
              state.controls.bordered),
            command_sink,
            app::SetBorder{state.session_id,
                           core::BorderPreference::Bordered});
    emit_if(radio_button(
              widget_label(i18n::TextId::Borderless, "borderless", language)
                .c_str(),
              state.settings.border == core::BorderPreference::Borderless,
              state.controls.borderless),
            command_sink,
            app::SetBorder{state.session_id,
                           core::BorderPreference::Borderless});

    ImGui::SeparatorText(
      i18n::text(i18n::TextId::Extensions, language).data());
    // Edit a fresh value snapshot each frame; only an accepted command updates
    // the next projected checkbox state.
    auto extensions = state.settings.extensions;
    const auto &extension_control = state.controls.browser_capture_keep_alive;
    ImGui::BeginDisabled(!extension_control.enabled);
    const auto keep_alive_label = widget_label(
      i18n::TextId::KeepAlive, "browser_keep_alive", language);
    const bool extension_changed = ImGui::Checkbox(
      keep_alive_label.c_str(), &extensions.browser_capture_keep_alive);
    ImGui::EndDisabled();
    // Keep the help marker outside the disabled block so it remains hoverable.
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort |
                             ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::BeginTooltip();
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0F);
      const auto help = i18n::text(i18n::TextId::KeepAliveHelp, language);
      ImGui::TextUnformatted(help.data(), help.data() + help.size());
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
    disabled_explanation(extension_control);
    literal_text(state.extensions_scope_text);
    emit_if(extension_changed, command_sink,
            app::SetExtensions{state.session_id, extensions});

    ImGui::SeparatorText(i18n::text(i18n::TextId::Hotkey, language).data());
    diagnostic(i18n::text(i18n::TextId::CurrentHotkey, language),
               std::string_view{state.hotkey_display_text});
    if (button(widget_label(i18n::TextId::RecordHotkey, "record_hotkey", language)
                 .c_str(), state.controls.replace_hotkey)) {
      hotkey_recording_ = HotkeyRecording{.session_id = state.session_id};
    }
    diagnostic(
      i18n::text(i18n::TextId::BindingLabel, language),
      i18n::text(state.hotkey_binding_confirmed ? i18n::TextId::Confirmed
                                                : i18n::TextId::Unconfirmed,
                 language));

  }

  void ControllerView::render_region(const app::ControllerViewState &state,
                                      const CommandSink &command_sink)
  {
    const auto language = state.effective_language;
    ImGui::SeparatorText(i18n::text(i18n::TextId::SourceRegion, language).data());
    if (!state.selected_source_id || !state.content_extent) {
      region_draft_.reset();
      literal_text(i18n::text(i18n::TextId::RegionAwaitingContent, language));
      if (state.selected_source_id) {
        emit_if(button(widget_label(i18n::TextId::CancelRegion, "cancel_region", language).c_str(),
                       state.controls.cancel_region_selection), command_sink,
                app::SetRegionSelection{state.session_id, *state.selected_source_id, false});
      }
      return;
    }

    const auto extent = *state.content_extent;
    const auto &source = *state.selected_source_id;
    constexpr auto max_coordinate = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    if (!region_draft_ || region_draft_->session_id != state.session_id ||
        region_draft_->source_id != source || region_draft_->extent != extent ||
        region_draft_->accepted_region != state.settings.region) {
      const auto region = state.settings.region.value_or(core::PixelRegion{0, 0, extent.width, extent.height});
      region_draft_ = RegionDraft{
        .session_id = state.session_id,
        .source_id = source,
        .extent = extent,
        .accepted_region = state.settings.region,
        .coordinates = {static_cast<int>(std::min(region.x, max_coordinate)),
                        static_cast<int>(std::min(region.y, max_coordinate)),
                        static_cast<int>(std::min(region.width, max_coordinate)),
                        static_cast<int>(std::min(region.height, max_coordinate))},
        .widget_scope = std::to_string(state.session_id.value) + ":" + source.value + ":" +
          std::to_string(extent.width) + "x" + std::to_string(extent.height),
      };
    }
    auto &draft = *region_draft_;
    // Scope changes detach any active ImGui text buffer from the old source.
    // Language changes keep the same IDs and preserve the current editing gesture.
    ImGui::PushID(draft.widget_scope.c_str());
    ImGui::Text("%s: %u x %u", i18n::text(i18n::TextId::SourceContentSize, language).data(),
                extent.width, extent.height);
    emit_if(button(widget_label(i18n::TextId::SelectRegion, "select_region", language).c_str(),
                   state.controls.region), command_sink,
            app::SetRegionSelection{state.session_id, source, true});
    if (state.settings.selecting_region) {
      emit_if(button(widget_label(i18n::TextId::CancelRegion, "cancel_region", language).c_str(),
                     state.controls.cancel_region_selection), command_sink,
              app::SetRegionSelection{state.session_id, source, false});
    }
    ImGui::PushTextWrapPos();
    literal_text(i18n::text(i18n::TextId::RegionHelp, language));
    ImGui::PopTextWrapPos();

    const bool numeric_enabled = state.controls.region.enabled &&
      extent.width <= max_coordinate && extent.height <= max_coordinate;
    ImGui::BeginDisabled(!numeric_enabled);
    constexpr std::array labels{i18n::TextId::RegionX, i18n::TextId::RegionY,
                               i18n::TextId::RegionWidth, i18n::TextId::RegionHeight};
    constexpr std::array ids{"region_x", "region_y", "region_width", "region_height"};
    for (std::size_t index = 0; index < draft.coordinates.size(); ++index) {
      ImGui::DragInt(widget_label(labels[index], ids[index], language).c_str(),
                     &draft.coordinates[index], 1.0F, 0, std::numeric_limits<int>::max(),
                     "%d", ImGuiSliderFlags_AlwaysClamp);
    }
    ImGui::EndDisabled();
    std::optional<core::PixelRegion> candidate;
    const auto &values = draft.coordinates;
    if (numeric_enabled && values[0] >= 0 && values[1] >= 0 && values[2] > 0 && values[3] > 0) {
      const core::PixelRegion region{static_cast<std::uint32_t>(values[0]),
                                     static_cast<std::uint32_t>(values[1]),
                                     static_cast<std::uint32_t>(values[2]),
                                     static_cast<std::uint32_t>(values[3])};
      if (core::valid_region(region, extent)) {
        candidate = region;
      }
    }
    const app::ControlState apply_control{
      .enabled = candidate.has_value(),
      .explanation = candidate ? std::string_view{} : i18n::text(i18n::TextId::RegionInvalid, language),
    };
    emit_if(button(widget_label(i18n::TextId::ApplyRegion, "apply_region", language).c_str(),
                   apply_control), command_sink, app::SetRegion{state.session_id, source, candidate});
    emit_if(button(widget_label(i18n::TextId::ResetRegion, "reset_region", language).c_str(),
                   state.controls.region), command_sink,
            app::SetRegion{state.session_id, source, std::nullopt});
    ImGui::PopID();
  }

  bool ControllerView::try_record_hotkey(core::HotkeyChord chord) noexcept
  {
    if (!hotkey_recording_) {
      return false;
    }
    if (!hotkey_recording_->waiting_for_release) {
      hotkey_recording_->candidate = chord;
    }
    return true;
  }

  void ControllerView::cancel_hotkey_recording() noexcept
  {
    hotkey_recording_.reset();
  }

  void ControllerView::render_hotkey_recorder(
    const app::ControllerViewState &state, const CommandSink &command_sink)
  {
    const auto language = state.effective_language;
    const auto label = widget_label(
      i18n::TextId::RecordHotkey, "hotkey_recorder", language);
    if (hotkey_recording_ && hotkey_recording_->open_requested) {
      ImGui::OpenPopup(label.c_str());
      hotkey_recording_->open_requested = false;
    }
    if (!ImGui::BeginPopupModal(label.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
      return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      cancel_hotkey_recording();
    }
    if (!hotkey_recording_) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }

    auto &recording = *hotkey_recording_;
    const auto &io = ImGui::GetIO();
    if (recording.waiting_for_release) {
      recording.waiting_for_release = !recording_inputs_released();
    } else {
      int primary_count{};
      std::optional<core::HotkeyKey> pressed;
      for (int value = ImGuiKey_Tab; value < ImGuiKey_GamepadStart; ++value) {
        if (value >= ImGuiKey_LeftCtrl && value <= ImGuiKey_RightSuper) {
          continue;
        }
        const auto key = static_cast<ImGuiKey>(value);
        primary_count += ImGui::IsKeyDown(key) ? 1 : 0;
        if (ImGui::IsKeyPressed(key, false)) {
          if (const auto mapped = portable_key(key)) {
            pressed = mapped;
          }
        }
      }
      if (primary_count > 1) {
        // A chord has one primary key. A release barrier gives an ambiguous
        // gesture a clean retry rather than choosing by enumeration order.
        recording.candidate.reset();
        recording.waiting_for_release = true;
      } else if (pressed) {
        recording.candidate = core::HotkeyChord{
          .modifiers = {.control = io.KeyCtrl, .alt = io.KeyAlt,
                        .shift = io.KeyShift, .meta = io.KeySuper},
          .key = *pressed};
      }
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0F);
    literal_text(i18n::text(recording.waiting_for_release
      ? i18n::TextId::ReleaseRecordingKeys : i18n::TextId::RecordHotkeyPrompt,
      language));
    ImGui::PopTextWrapPos();
    if (recording.candidate) {
      literal_text(core::hotkey_display_text(*recording.candidate));
    }
    ImGui::BeginDisabled(!recording.candidate || recording.waiting_for_release);
    const bool confirm = ImGui::Button(
      widget_label(i18n::TextId::ConfirmHotkey, "confirm_hotkey", language).c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button(
      widget_label(i18n::TextId::CancelHotkey, "cancel_hotkey", language).c_str());
    if (confirm && recording.candidate) {
      emit_if(true, command_sink, app::ReplaceHotkey{
        recording.session_id, *recording.candidate});
    }
    if (confirm || cancel) {
      cancel_hotkey_recording();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

}    // namespace avb::ui
