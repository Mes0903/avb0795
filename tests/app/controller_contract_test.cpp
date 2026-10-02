#include "avb/app/controller_view_state.hpp"

#include "avb/app/config_store.hpp"
#include "avb/app/localized_presentation.hpp"
#include "avb/core/application_state.hpp"
#include "avb/i18n/catalog.hpp"
#include "avb/i18n/language.hpp"
#include "avb/ui/controller_view.hpp"
#include "src/desktop/imgui_input_sync.hpp"
#include "src/desktop/ui_cadence.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#if defined(_WIN32)
#include "platform/windows/controller_font.hpp"
#include "src/desktop/controller_font.hpp"
#endif

#include <array>
#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

  using avb::app::BridgeFeedback;
  using avb::app::ControllerViewState;
  using avb::app::RuntimeFeedback;
  using avb::core::AppError;
  using avb::core::AppErrorCode;
  using avb::core::ApplicationState;
  using avb::core::BridgeMode;
  using avb::core::BridgeReason;
  using avb::core::CaptureExclusionStatus;
  using avb::core::ColorSupport;
  using avb::core::PlatformCapabilities;
  using avb::core::RuntimeDrainStatus;
  using avb::core::SessionLifecycle;
  using avb::core::SourceDescriptor;
  using avb::core::SourceId;
  using avb::core::SourceKind;

  constexpr PlatformCapabilities all_capabilities{
    .display_capture = true,
    .window_capture = true,
    .pass_through = true,
    .borderless_resize = true,
    .global_hotkey = true,
    .capture_exclusion = true,
    .browser_capture_keep_alive = true,
  };

  [[nodiscard]] SourceDescriptor source(std::string_view id, SourceKind kind, ColorSupport color_support)
  {
    return SourceDescriptor{
      .id = SourceId{std::string{id}},
      .kind = kind,
      .display_label = kind == SourceKind::Display ? "Display 1" : "Editor",
      .extent = {.width = 1920, .height = 1080},
      .color_support = color_support,
    };
  }

  [[nodiscard]] ControllerViewState
  project(const ApplicationState &state,
          std::span<const SourceDescriptor> sources = {},
          PlatformCapabilities capabilities = all_capabilities,
          CaptureExclusionStatus controller_exclusion =
            CaptureExclusionStatus::Excluded,
          RuntimeFeedback runtime_feedback = {},
          bool runtime_present = false,
          bool stop_requested = false,
          std::optional<AppError> notice = std::nullopt,
          bool hotkey_binding_confirmed = true,
          const avb::app::UserConfig &config = {},
          avb::i18n::Language language =
            avb::i18n::Language::English,
          bool language_changes_allowed = true)
  {
    return avb::app::build_controller_view(
      state, state.current_session_id(), sources, capabilities,
      config,
      hotkey_binding_confirmed, controller_exclusion, runtime_feedback,
      runtime_present, stop_requested, notice, language,
      language_changes_allowed);
  }

  [[nodiscard]] ApplicationState state_in(SessionLifecycle lifecycle)
  {
    ApplicationState state;
    const auto session_id = state.current_session_id();
    state.select_source(
           session_id,
           source("display", SourceKind::Display, ColorSupport::Sdr))
      .value();

    if (lifecycle == SessionLifecycle::Idle) {
      return state;
    }

    state.begin_start(session_id).value();
    if (lifecycle == SessionLifecycle::Starting) {
      return state;
    }

    state.mark_capturing(session_id).value();
    switch (lifecycle) {
    case SessionLifecycle::Capturing:
      break;
    case SessionLifecycle::Stopping:
      state.begin_stop(session_id).value();
      break;
    case SessionLifecycle::Interrupted:
      state.mark_interrupted(session_id).value();
      break;
    case SessionLifecycle::RecoveringGraphics:
      state.begin_graphics_recovery(session_id).value();
      break;
    case SessionLifecycle::Error:
      state.mark_error(session_id).value();
      break;
    case SessionLifecycle::Idle:
    case SessionLifecycle::Starting:
      break;
    }
    return state;
  }

  template <typename Value>
  concept has_thumbnail = requires(const Value &value) { value.thumbnail; };

  template <typename Value>
  concept has_pixels = requires(const Value &value) { value.pixels; };

  template <typename Value>
  concept has_pixel_data = requires(const Value &value) { value.pixel_data; };

}    // namespace

TEST_CASE("avb0795 V1 case 07: Controller projection contract",
          "[avb0795-v1]")
{
  SECTION("UI cadence separates progress wakes from active and idle deadlines")
  {
    using namespace std::chrono_literals;
    using avb::desktop::detail::HostTime;
    using avb::desktop::detail::UiCadence;
    using avb::desktop::detail::UiTickMode;
    constexpr auto active_period = 16'666'667ns;
    const auto now = HostTime{1s};
    UiCadence cadence;

    CHECK(cadence.decision(now).mode == UiTickMode::Visible);
    cadence.did_build(now);
    // Retirement wakes query the policy; user activity enters through note_input.
    for (auto elapsed = 2ms; elapsed < 100ms; elapsed += 2ms) {
      const auto decision = cadence.decision(now + elapsed);
      CHECK(decision.mode == UiTickMode::None);
      CHECK(decision.next_deadline == now + 100ms);
    }
    CHECK(cadence.decision(now + 100ms).mode == UiTickMode::Visible);

    // A delayed build establishes a fresh period instead of catching up ticks.
    cadence.did_build(now + 350ms);
    CHECK(cadence.decision(now + 350ms).mode == UiTickMode::None);
    CHECK(cadence.decision(now + 350ms).next_deadline == now + 450ms);
    cadence.note_input(now + 351ms);
    const auto due = now + 350ms + active_period;
    CHECK(cadence.decision(due - 1ns).mode == UiTickMode::None);
    CHECK(cadence.decision(due - 1ns).next_deadline == due);
    CHECK(cadence.decision(due).mode == UiTickMode::Visible);
    cadence.did_build(due);
    CHECK(cadence.decision(due).mode == UiTickMode::None);

    // The 500 ms grace ends at its boundary; held/editing facts keep active cadence.
    cadence.did_build(now + 851ms);
    CHECK(cadence.decision(now + 852ms).next_deadline == now + 951ms);
    CHECK(cadence.decision(now + 852ms, true).next_deadline ==
          now + 851ms + active_period);
    CHECK(cadence.decision(now + 851ms + active_period, true).mode ==
          UiTickMode::Visible);
  }

  SECTION("hidden cadence consumes pending input once and restores immediately")
  {
    using namespace std::chrono_literals;
    using avb::desktop::detail::HostTime;
    using avb::desktop::detail::UiCadence;
    using avb::desktop::detail::UiTickMode;
    const auto now = HostTime{1s};
    UiCadence cadence;
    cadence.did_build(now);

    // Native input can arrive before the iconify callback in the same poll batch.
    cadence.note_input(now + 1ms);
    cadence.set_visible(false, now + 2ms);
    CHECK(cadence.decision(now + 2ms).mode == UiTickMode::HiddenSync);
    cadence.did_build(now + 2ms);
    const auto hidden = cadence.decision(now + 2s, true);
    CHECK(hidden.mode == UiTickMode::None);
    CHECK_FALSE(hidden.next_deadline.has_value());

    cadence.note_input(now + 3s);
    CHECK(cadence.decision(now + 3s).mode == UiTickMode::HiddenSync);
    cadence.did_build(now + 3s);
    CHECK(cadence.decision(now + 3s).mode == UiTickMode::None);
    CHECK_FALSE(cadence.decision(now + 3s).next_deadline.has_value());

    cadence.set_visible(true, now + 3s + 1ms);
    CHECK(cadence.decision(now + 3s + 1ms).mode == UiTickMode::Visible);
    cadence.did_build(now + 3s + 1ms);
    CHECK(cadence.decision(now + 3s + 1ms).mode == UiTickMode::None);
  }

  SECTION("hidden ImGui sync drains same-batch release and focus before idle")
  {
    using namespace std::chrono_literals;
    using avb::desktop::detail::HostTime;
    using avb::desktop::detail::UiCadence;
    using avb::desktop::detail::UiTickMode;
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{
      ImGui::CreateContext(), &ImGui::DestroyContext};
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {640.0F, 720.0F};
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto now = HostTime{1s};
    UiCadence cadence;
    io.AddKeyEvent(ImGuiKey_B, true);
    ImGui::NewFrame();
    REQUIRE(ImGui::IsKeyDown(ImGuiKey_B));
    ImGui::EndFrame();
    cadence.did_build(now);

    io.ConfigInputTrickleEventQueue = true;
    io.AddKeyEvent(ImGuiKey_A, true);
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddMouseButtonEvent(0, true);
    io.AddMouseButtonEvent(0, false);
    io.AddFocusEvent(false);
    cadence.note_input(now + 1ms);
    cadence.set_visible(false, now + 1ms);
    REQUIRE(cadence.decision(now + 1ms).mode == UiTickMode::HiddenSync);
    REQUIRE_FALSE(context->InputEventsQueue.empty());
    int backend_frames{};
    avb::desktop::detail::synchronize_hidden_imgui_input([&] {
      ++backend_frames;
      CHECK_FALSE(io.ConfigInputTrickleEventQueue);
    });
    cadence.did_build(now + 1ms);
    CHECK(backend_frames == 1);
    CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_A));
    // Focus loss clears a key that has no explicit release in this batch.
    CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_B));
    CHECK_FALSE(ImGui::IsMouseDown(0));
    CHECK(context->InputEventsQueue.empty());
    CHECK(io.ConfigInputTrickleEventQueue);
    CHECK(cadence.decision(now + 10s).mode == UiTickMode::None);
    CHECK_FALSE(cadence.decision(now + 10s).next_deadline.has_value());

    // Restore uses the visible frame flow and the saved trickling preference.
    io.AddFocusEvent(true);
    cadence.note_input(now + 11s);
    cadence.set_visible(true, now + 11s);
    REQUIRE(cadence.decision(now + 11s).mode == UiTickMode::Visible);
    ImGui::NewFrame();
    CHECK_FALSE(io.AppFocusLost);
    CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_A));
    ImGui::EndFrame();
    CHECK(io.ConfigInputTrickleEventQueue);

    io.ConfigInputTrickleEventQueue = false;
    avb::desktop::detail::synchronize_hidden_imgui_input([] {});
    CHECK_FALSE(io.ConfigInputTrickleEventQueue);
  }

  SECTION("source rows use typed columns and compact disabled feedback")
  {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{
      ImGui::CreateContext(), &ImGui::DestroyContext};
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {640.0F, 1200.0F};
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    avb::ui::ControllerView controller;
    std::array sources{
      source("window", SourceKind::Window, ColorSupport::Sdr),
      source("display", SourceKind::Display, ColorSupport::Sdr)};
    sources[0].display_label = "Editor 100% ##literal###title";
    sources[0].native_handle_text = "0x123456789ABCDEF0";
    auto view = project(ApplicationState{}, sources);
    for (int frame{}; frame < 2; ++frame) {
      ImGui::NewFrame();
      controller.render(view, {});
      ImGui::Render();
    }
    auto *const root = ImGui::FindWindowByName("avb0795 Controller");
    REQUIRE(root != nullptr);
    REQUIRE(root->DC.ChildWindows.Size == 1);
    auto *const content = root->DC.ChildWindows[0];
    const auto *const table = ImGui::TableFindByID(content->GetID("sources_table"));
    REQUIRE(table != nullptr);
    CHECK(table->ColumnsCount == 3);
    CHECK(view.sources[0].kind == SourceKind::Window);
    CHECK(view.sources[0].native_handle_text == sources[0].native_handle_text);
    const auto has_color = [&](ImVec4 color) {
      const auto expected = ImGui::GetColorU32(color);
      return std::ranges::any_of(content->DrawList->VtxBuffer,
        [expected](const auto &vertex) { return vertex.col == expected; });
    };
    CHECK(has_color({1.0F, 0.45F, 0.7F, 1.0F}));
    CHECK(has_color({0.65F, 0.6F, 1.0F, 1.0F}));
    CHECK(has_color({0.4F, 0.9F, 0.55F, 1.0F}));
    std::vector<avb::app::AppCommand> commands;
    const auto sink = [&](auto command) { commands.push_back(std::move(command)); };
    const auto row_id = ImHashStr("##source", 0,
      ImHashStr(sources[0].id.value.c_str(), 0, table->ID));
    ImGui::ActivateItemByID(row_id);
    ImGui::NewFrame();
    controller.render(view, sink);
    ImGui::Render();
    REQUIRE(commands.size() == 1);
    const auto &selection = std::get<avb::app::SelectSource>(commands.front());
    CHECK(selection.source_id == sources[0].id);
    CHECK(selection.session_id == view.session_id);
    commands.clear();
    ImGui::ActivateItemByID(content->GetID("###auto_refresh"));
    ImGui::NewFrame();
    controller.render(view, sink);
    ImGui::Render();
    REQUIRE(commands.size() == 1);
    CHECK(std::get<avb::app::SetAutoRefresh>(commands.front()).enabled);
    commands.clear();
    // Logging observes complete literal labels independently of glyph clipping.
    for (auto &row : view.sources) {
      row.select = {.enabled = false, .explanation = "REPEATED_ROW_REASON"};
    }
    ImGui::ActivateItemByID(row_id);
    ImGui::NewFrame();
    ImGui::LogToBuffer();
    controller.render(view, sink);
    const std::string logged{ImGui::GetCurrentContext()->LogBuffer.c_str()};
    ImGui::LogFinish();
    CHECK(logged.find(sources[0].display_label) != std::string::npos);
    CHECK(logged.find(sources[0].native_handle_text) != std::string::npos);
    CHECK(logged.find("REPEATED_ROW_REASON") == std::string::npos);
    // The title uses a vector glyph, so a textual Quit here would be the footer.
    CHECK(logged.find("Quit") == std::string::npos);
    CHECK(commands.empty());
    ImGui::Render();
  }

  SECTION("source column borders resize without selecting a source")
  {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{
      ImGui::CreateContext(), &ImGui::DestroyContext};
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800.0F, 1200.0F};
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    avb::ui::ControllerView controller;
    std::array sources{
      source("window", SourceKind::Window, ColorSupport::Sdr),
      source("display", SourceKind::Display, ColorSupport::Sdr)};
    sources[0].native_handle_text = "0x123456789ABCDEF0";
    const auto view = project(ApplicationState{}, sources);
    std::vector<avb::app::AppCommand> commands;
    const auto draw = [&] {
      ImGui::NewFrame();
      controller.render(view, [&](auto command) {
        commands.push_back(std::move(command));
      });
      ImGui::Render();
    };
    draw();
    draw();
    auto *const root = ImGui::FindWindowByName("avb0795 Controller");
    REQUIRE(root != nullptr);
    REQUIRE(root->DC.ChildWindows.Size == 1);
    auto *const content = root->DC.ChildWindows[0];
    const auto *const table = ImGui::TableFindByID(content->GetID("sources_table"));
    REQUIRE(table != nullptr);
    REQUIRE((table->Flags & ImGuiTableFlags_Resizable) != 0);
    for (const int border : {0, 1}) {
      CAPTURE(border);
      const auto previous_width = table->Columns[border].WidthGiven;
      const ImVec2 start{table->Columns[border].MaxX,
                        table->OuterRect.Min.y + 5.0F};
      io.AddMousePosEvent(start.x, start.y);
      draw();
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      draw();
      io.AddMousePosEvent(start.x + 36.0F, start.y);
      draw();
      draw();
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      draw();
      const auto resized_width = table->Columns[border].WidthGiven;
      CHECK(resized_width > previous_width + 25.0F);
      draw();
      CHECK(table->Columns[border].WidthGiven == resized_width);
      CHECK(commands.empty());
    }
  }

  SECTION("wheel zoom draft commits once and cancels with its session or availability")
  {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{
      ImGui::CreateContext(), &ImGui::DestroyContext};
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800.0F, 2000.0F};
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    avb::ui::ControllerView controller;
    auto view = project(ApplicationState{});
    std::vector<avb::app::AppCommand> commands;
    const auto draw = [&] {
      ImGui::NewFrame();
      controller.render(view, [&](auto command) {
        commands.push_back(std::move(command));
      });
      ImGui::Render();
    };
    draw();
    draw();
    auto *const root = ImGui::FindWindowByName("avb0795 Controller");
    REQUIRE(root != nullptr);
    REQUIRE(root->DC.ChildWindows.Size == 1);
    auto *const content = root->DC.ChildWindows[0];
    const auto wheel_id = [&] {
      const auto identity = "###wheel_zoom_" + std::to_string(view.session_id.value) +
        (view.controls.wheel_zoom.enabled ? "_enabled" : "_disabled");
      return content->GetID(identity.c_str());
    };
    // Keyboard activation exposes the actual widget rectangle for subsequent
    // ImGui-local mouse input; the test never sends events to a native window.
    ImGui::ActivateItemByID(wheel_id());
    context->NavNextActivateFlags = ImGuiActivateFlags_PreferTweak;
    draw();
    REQUIRE(ImGui::GetActiveID() == wheel_id());
    CHECK(controller.has_active_interaction());
    auto rectangle = content->NavRectRel[ImGuiNavLayer_Main];
    rectangle.Translate(content->Pos);
    ImGui::ActivateItemByID(wheel_id());
    context->NavNextActivateFlags = ImGuiActivateFlags_PreferTweak;
    draw();
    CHECK_FALSE(controller.has_active_interaction());
    CHECK(commands.empty());
    const float y = rectangle.GetCenter().y;
    io.AddMousePosEvent(rectangle.Min.x + rectangle.GetWidth() * 0.6F, y);
    draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    draw();
    REQUIRE(ImGui::GetActiveID() == wheel_id());
    CHECK(controller.has_active_interaction());
    CHECK(commands.empty());
    io.AddMousePosEvent(rectangle.Min.x + rectangle.GetWidth() * 0.8F, y);
    draw();
    CHECK(commands.empty());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    draw();
    draw();
    REQUIRE(commands.size() == 1);
    const auto *const dragged = std::get_if<avb::app::SetWheelZoomPercent>(&commands[0]);
    REQUIRE(dragged != nullptr);
    CHECK(dragged->session_id == view.session_id);
    CHECK(dragged->percent > 5);
    CHECK(dragged->percent <= 25);
    CHECK_FALSE(controller.has_active_interaction());
    commands.clear();

    const auto open_numeric = [&] {
      io.AddMousePosEvent(rectangle.GetCenter().x, y);
      io.AddKeyEvent(ImGuiMod_Ctrl, true);
      draw();
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      draw();
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      io.AddKeyEvent(ImGuiMod_Ctrl, false);
      draw();
      // ImGui trickles a modifier change behind the mouse release into the next
      // frame, matching the controller's normal visible input cadence.
      draw();
      REQUIRE_FALSE(io.KeyCtrl);
      REQUIRE(context->InputEventsQueue.empty());
      REQUIRE(ImGui::GetActiveID() == wheel_id());
      auto *const input = ImGui::GetInputTextState(wheel_id());
      REQUIRE(input != nullptr);
      CHECK(controller.has_active_interaction());
      return std::string{input->TextA.Data};
    };
    const auto check_commit = [&](int percent) {
      REQUIRE(commands.size() == 1);
      const auto *const command = std::get_if<avb::app::SetWheelZoomPercent>(&commands[0]);
      REQUIRE(command != nullptr);
      CHECK(command->session_id == view.session_id);
      CHECK(command->percent == percent);
      CHECK_FALSE(controller.has_active_interaction());
      commands.clear();
    };
    // The accepted snapshot remains 5, modelling a rejected previous command.
    CHECK(open_numeric() == "5");
    io.AddInputCharactersUTF8("12");
    draw();
    REQUIRE(std::string{ImGui::GetInputTextState(wheel_id())->TextA.Data} == "12");
    CHECK(commands.empty());
    io.AddFocusEvent(false);
    avb::desktop::detail::synchronize_hidden_imgui_input([] {});
    avb::desktop::detail::synchronize_hidden_imgui_input([] {});
    io.AddFocusEvent(true);
    draw();
    draw();
    check_commit(12);

    for (const auto &[text, expected] : std::array{
           std::pair{"0", 1}, std::pair{"99", 25}}) {
      CAPTURE(text);
      CHECK(open_numeric() == "5");
      io.AddInputCharactersUTF8(text);
      draw();
      CHECK(commands.empty());
      io.AddKeyEvent(ImGuiKey_Enter, true);
      draw();
      io.AddKeyEvent(ImGuiKey_Enter, false);
      draw();
      check_commit(expected);
    }
    for (const bool session_changed : {true, false}) {
      CAPTURE(session_changed);
      CHECK(open_numeric() == "5");
      io.AddInputCharactersUTF8("17");
      draw();
      CHECK(commands.empty());
      if (session_changed) {
        ++view.session_id.value;
      } else {
        view.controls.wheel_zoom.enabled = false;
      }
      draw();
      draw();
      CHECK(commands.empty());
      CHECK_FALSE(controller.has_active_interaction());
      view.controls.wheel_zoom.enabled = true;
      draw();
    }
    CHECK(open_numeric() == "5");
  }

  SECTION("hotkey recorder captures values and cancels stale input")
  {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{
      ImGui::CreateContext(), &ImGui::DestroyContext};
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {720.0F, 2000.0F};
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    avb::ui::ControllerView controller;
    auto view = project(ApplicationState{});
    std::vector<avb::app::AppCommand> commands;
    const auto draw = [&] {
      ImGui::NewFrame();
      controller.render(view, [&](auto command) {
        commands.push_back(std::move(command));
      });
      ImGui::Render();
    };
    draw();
    draw();
    auto *const root = ImGui::FindWindowByName("avb0795 Controller");
    REQUIRE(root != nullptr);
    REQUIRE(root->DC.ChildWindows.Size == 1);
    auto *const content = root->DC.ChildWindows[0];
    ImGui::ActivateItemByID(content->GetID("###record_hotkey"));
    draw();
    REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
    CHECK(commands.empty());
    const avb::core::HotkeyChord existing{
      .modifiers = {.control = true, .alt = true},
      .key = avb::core::HotkeyKey::F8};
    draw();
    const auto confirm = [&] {
      auto *popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
      REQUIRE(popup != nullptr);
      ImGui::ActivateItemByID(popup->GetID("###confirm_hotkey"));
      draw();
    };
    const auto open = [&] {
      ImGui::ActivateItemByID(content->GetID("###record_hotkey"));
      draw();
      draw();
      REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
    };
    CHECK(controller.try_record_hotkey(existing));
    CHECK(commands.empty());
    confirm();
    REQUIRE(commands.size() == 1);
    const auto &native_command =
      std::get<avb::app::ReplaceHotkey>(commands.front());
    CHECK(native_command.hotkey == existing);
    CHECK(native_command.session_id == view.session_id);
    CHECK_FALSE(controller.try_record_hotkey(existing));
    commands.clear();

    // Exercise all three portable ranges through actual ImGui key events.
    for (const auto &[key, mapped] : std::array{
      std::pair{ImGuiKey_A, avb::core::HotkeyKey::A},
      std::pair{ImGuiKey_0, avb::core::HotkeyKey::Digit0},
      std::pair{ImGuiKey_F12, avb::core::HotkeyKey::F12}}) {
      open();
      io.AddKeyEvent(ImGuiMod_Ctrl, true);
      io.AddKeyEvent(ImGuiMod_Shift, true);
      io.AddKeyEvent(key, true);
      draw();
      draw(); // A held key leaves the same candidate awaiting explicit confirmation.
      CHECK(commands.empty());
      io.AddKeyEvent(key, false);
      io.AddKeyEvent(ImGuiMod_Ctrl, false);
      io.AddKeyEvent(ImGuiMod_Shift, false);
      draw();
      confirm();
      REQUIRE(commands.size() == 1);
      const auto &typed = std::get<avb::app::ReplaceHotkey>(commands.front());
      CHECK(typed.hotkey.key == mapped);
      CHECK(typed.hotkey.modifiers.control);
      CHECK(typed.hotkey.modifiers.shift);
      commands.clear();
    }

    // The opening gesture and a simultaneous two-primary gesture each need release.
    io.AddKeyEvent(ImGuiKey_Enter, true);
    open();
    CHECK(controller.try_record_hotkey(existing));
    io.AddKeyEvent(ImGuiKey_Enter, false);
    draw();
    confirm();
    CHECK(commands.empty());
    io.ConfigInputTrickleEventQueue = false;
    io.AddKeyEvent(ImGuiKey_A, true);
    io.AddKeyEvent(ImGuiKey_B, true);
    draw();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiKey_B, false);
    draw();
    confirm();
    CHECK(commands.empty());
    controller.cancel_hotkey_recording();
    draw();
    io.ConfigInputTrickleEventQueue = true;

    open();
    CHECK(controller.try_record_hotkey(existing));
    io.AddFocusEvent(false);
    // The host's focus callback invokes this before forwarding its queued event.
    // Cancellation is context-free and remains effective when focus regains in
    // the same poll batch, whose final ImGui focus value becomes true again.
    controller.cancel_hotkey_recording();
    CHECK_FALSE(controller.try_record_hotkey(existing));
    io.AddFocusEvent(true);
    draw();
    CHECK(commands.empty());
    CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0);

    for (int reason{}; reason < 5; ++reason) {
      CAPTURE(reason);
      open();
      CHECK(controller.try_record_hotkey(existing));
      if (reason == 0) {
        io.AddKeyEvent(ImGuiKey_Escape, true);
      } else if (reason == 1) {
        ++view.session_id.value;
      } else if (reason == 2) {
        view.controls.replace_hotkey.enabled = false;
      } else if (reason == 3) {
        io.AddFocusEvent(false);
      } else {
        auto *popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
        ImGui::ActivateItemByID(popup->GetID("###cancel_hotkey"));
      }
      draw();
      CHECK(commands.empty());
      CHECK_FALSE(controller.try_record_hotkey(existing));
      CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0);
      io.AddKeyEvent(ImGuiKey_Escape, false);
      io.AddFocusEvent(true);
      view.controls.replace_hotkey.enabled = true;
      draw();
    }
  }

  SECTION("opacity input previews commits and retains its originating session")
  {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{
      ImGui::CreateContext(), &ImGui::DestroyContext};
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {720.0F, 2000.0F};
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    avb::ui::ControllerView controller;
    auto view = project(ApplicationState{});
    const auto originating_session = view.session_id;
    std::vector<avb::app::AppCommand> commands;
    const auto draw = [&] {
      ImGui::NewFrame();
      controller.render(view, [&](auto command) {
        commands.push_back(std::move(command));
      });
      ImGui::Render();
    };
    draw();
    draw();
    auto *const root = ImGui::FindWindowByName("avb0795 Controller");
    REQUIRE(root != nullptr);
    REQUIRE(root->DC.ChildWindows.Size == 1);
    auto *const content = root->DC.ChildWindows[0];
    const auto opacity_id = content->GetID("###opacity");
    const auto open_input = [&] {
      ImGui::ActivateItemByID(opacity_id);
      context->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
      draw();
      REQUIRE(ImGui::GetActiveID() == opacity_id);
      auto *const input = ImGui::GetInputTextState(opacity_id);
      REQUIRE(input != nullptr);
      return std::string{input->TextA.Data};
    };
    const auto confirm_input = [&] {
      io.AddKeyEvent(ImGuiKey_Enter, true);
      draw();
      io.AddKeyEvent(ImGuiKey_Enter, false);
      draw();
      CHECK(ImGui::GetActiveID() != opacity_id);
    };
    const auto check_emission = [&](int expected_percent) {
      // One typed replacement must preview before committing. The application
      // consumes the complete command values after this UI frame finishes.
      REQUIRE(commands.size() == 2);
      const auto *const preview =
        std::get_if<avb::app::SetOpacity>(&commands[0]);
      const auto *const commit =
        std::get_if<avb::app::SetOpacity>(&commands[1]);
      REQUIRE(preview != nullptr);
      REQUIRE(commit != nullptr);
      CHECK(preview->session_id == originating_session);
      CHECK(preview->percent == expected_percent);
      CHECK_FALSE(preview->commit);
      CHECK(commit->session_id == originating_session);
      CHECK(commit->commit);
    };

    SECTION("visible slider previews mouse positions and commits once on release")
    {
      // Navigation exposes the widget's rectangle without native input. Mouse
      // events below stay in this headless ImGui context.
      ImGui::ActivateItemByID(opacity_id);
      context->NavNextActivateFlags = ImGuiActivateFlags_PreferTweak;
      draw();
      REQUIRE(ImGui::GetActiveID() == opacity_id);
      auto rectangle = content->NavRectRel[ImGuiNavLayer_Main];
      rectangle.Translate(content->Pos);
      ImGui::ActivateItemByID(opacity_id);
      context->NavNextActivateFlags = ImGuiActivateFlags_PreferTweak;
      draw();
      REQUIRE_FALSE(controller.has_active_interaction());
      REQUIRE(commands.empty());

      const float y = rectangle.GetCenter().y;
      io.AddMousePosEvent(rectangle.Min.x + rectangle.GetWidth() * 0.25F, y);
      draw();
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      draw();
      REQUIRE(ImGui::GetActiveID() == opacity_id);
      CHECK(controller.has_active_interaction());
      // Clicking the track previews its absolute position immediately.
      REQUIRE(commands.size() == 1);
      const auto *const first = std::get_if<avb::app::SetOpacity>(&commands[0]);
      REQUIRE(first != nullptr);
      CHECK(first->session_id == originating_session);
      CHECK_FALSE(first->commit);
      REQUIRE(first->percent > 0);
      REQUIRE(first->percent < 50);
      const int first_percent = first->percent;
      view.settings.opacity_percent = first_percent;

      io.AddMousePosEvent(rectangle.Min.x + rectangle.GetWidth() * 0.7F, y);
      draw();
      REQUIRE(commands.size() == 2);
      const auto *const second = std::get_if<avb::app::SetOpacity>(&commands[1]);
      REQUIRE(second != nullptr);
      CHECK(second->session_id == originating_session);
      CHECK_FALSE(second->commit);
      REQUIRE(second->percent > first_percent);
      REQUIRE(second->percent < 100);
      const int accepted_percent = second->percent;
      view.settings.opacity_percent = accepted_percent;

      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      draw();
      draw();
      REQUIRE(commands.size() == 3);
      const auto *const commit = std::get_if<avb::app::SetOpacity>(&commands[2]);
      REQUIRE(commit != nullptr);
      CHECK(commit->session_id == originating_session);
      CHECK(commit->percent == accepted_percent);
      CHECK(commit->commit);
      CHECK_FALSE(controller.has_active_interaction());
    }

    SECTION("typed edits retain their session and rejected previews reset")
    {
      CHECK(open_input() == "100");
      io.AddInputCharactersUTF8("37");
      draw();
      confirm_input();
      check_emission(37);

      // Keeping the accepted snapshot at 100 models rejection of the preview.
      // A new gesture must start from that accepted value, not the rejected 37.
      commands.clear();
      CHECK(open_input() == "100");
      ++view.session_id.value;
      view.effective_language = avb::i18n::Language::Japanese;
      draw();
      CHECK(ImGui::GetActiveID() == opacity_id);
      io.AddInputCharactersUTF8("62");
      draw();
      confirm_input();
      check_emission(62);
    }

    SECTION("active numeric draft survives repeated hidden input sync")
    {
      const int hidden_sync_count = GENERATE(2, 3, 8);
      CAPTURE(hidden_sync_count);
      CHECK(open_input() == "100");
      io.AddInputCharactersUTF8("48");
      draw();
      REQUIRE(commands.size() == 1);
      const auto &preview = std::get<avb::app::SetOpacity>(commands.front());
      REQUIRE(preview.percent == 48);
      REQUIRE_FALSE(preview.commit);
      view.settings.opacity_percent = 48;
      io.AddFocusEvent(false);
      avb::desktop::detail::synchronize_hidden_imgui_input([] {});
      io.AddMousePosEvent(0.0F, 0.0F);
      avb::desktop::detail::synchronize_hidden_imgui_input([] {});
      for (int index = 2; index < hidden_sync_count; ++index) {
        io.AddMousePosEvent(static_cast<float>(index), 1.0F);
        avb::desktop::detail::synchronize_hidden_imgui_input([] {});
      }
      io.AddFocusEvent(true);
      draw();
      draw();
      check_emission(48);
    }

    SECTION("numeric Escape previews the original value before committing")
    {
      CHECK(open_input() == "100");
      io.AddInputCharactersUTF8("48");
      draw();
      REQUIRE(commands.size() == 1);
      CHECK(std::get<avb::app::SetOpacity>(commands.front()).percent == 48);
      view.settings.opacity_percent = 48;
      commands.clear();
      io.AddKeyEvent(ImGuiKey_Escape, true);
      draw();
      io.AddKeyEvent(ImGuiKey_Escape, false);
      draw();
      check_emission(100);
    }

    SECTION("accepted previews commit after hidden focus and release sync")
    {
      const auto [hidden_sync_count, prior_numeric_edit] = GENERATE(
        std::make_pair(2, false), std::make_pair(3, false),
        std::make_pair(2, true));
      CAPTURE(hidden_sync_count, prior_numeric_edit);
      if (prior_numeric_edit) {
        CHECK(open_input() == "100");
        io.AddInputCharactersUTF8("62");
        draw();
        confirm_input();
        check_emission(62);
        view.settings.opacity_percent = 62;
        commands.clear();
      }
      // A live keyboard drag previews before release. Model acceptance in the
      // next snapshot, then minimize while the gesture is still active.
      ImGui::ActivateItemByID(opacity_id);
      context->NavNextActivateFlags = ImGuiActivateFlags_PreferTweak;
      draw();
      REQUIRE(ImGui::GetActiveID() == opacity_id);
      io.AddKeyEvent(ImGuiKey_LeftArrow, true);
      draw();
      REQUIRE(commands.size() == 1);
      const auto *const hidden_preview =
        std::get_if<avb::app::SetOpacity>(&commands.front());
      REQUIRE(hidden_preview != nullptr);
      REQUIRE_FALSE(hidden_preview->commit);
      REQUIRE(hidden_preview->percent < 100);
      const int accepted_percent = hidden_preview->percent;
      view.settings.opacity_percent = accepted_percent;

      io.AddFocusEvent(false);
      avb::desktop::detail::synchronize_hidden_imgui_input([] {});
      io.AddKeyEvent(ImGuiKey_LeftArrow, false);
      avb::desktop::detail::synchronize_hidden_imgui_input([] {});
      if (hidden_sync_count == 3) {
        io.AddMousePosEvent(0.0F, 0.0F);
        avb::desktop::detail::synchronize_hidden_imgui_input([] {});
      }
      CHECK(commands.size() == 1);
      CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_LeftArrow));
      io.AddFocusEvent(true);
      draw();
      const auto commands_after_first_restore = commands.size();
      draw();
      std::string hidden_emissions;
      for (const auto &command : commands) {
        if (const auto *opacity = std::get_if<avb::app::SetOpacity>(&command)) {
          hidden_emissions += std::to_string(opacity->percent) +
            (opacity->commit ? ":commit " : ":preview ");
        }
      }
      CAPTURE(commands_after_first_restore, hidden_emissions);
      check_emission(accepted_percent);
      commands.clear();
      CHECK(open_input() == std::to_string(accepted_percent));
    }
  }

  SECTION("region actions retain exact session source and extent identities")
  {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{
      ImGui::CreateContext(), &ImGui::DestroyContext};
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {720.0F, 2400.0F};
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto state = state_in(SessionLifecycle::Capturing);
    auto view = project(state, {}, all_capabilities, CaptureExclusionStatus::Excluded,
                        RuntimeFeedback{.content_extent = avb::core::Extent{200, 100}}, true);
    REQUIRE(view.selected_source_id == SourceId{"display"});
    REQUIRE(view.controls.region.enabled);
    avb::ui::ControllerView controller;
    std::vector<avb::app::AppCommand> commands;
    const auto draw = [&] {
      ImGui::NewFrame();
      controller.render(view, [&](auto command) { commands.push_back(std::move(command)); });
      ImGui::Render();
    };
    draw();
    draw();
    auto *const root = ImGui::FindWindowByName("avb0795 Controller");
    REQUIRE(root != nullptr);
    auto *const content = root->DC.ChildWindows[0];
    const auto region_id = [&](const char *widget) {
      const auto key = std::to_string(view.session_id.value) + ":" +
        view.selected_source_id->value + ":" + std::to_string(view.content_extent->width) +
        "x" + std::to_string(view.content_extent->height);
      return ImHashStr(widget, 0, ImHashStr(key.c_str(), 0, content->IDStack.back()));
    };
    // The coordinate editor and its submission action have been removed.
    ImGui::ActivateItemByID(region_id("###apply_region"));
    draw();
    REQUIRE(commands.empty());

    const int identity_change = GENERATE(0, 1, 2, 3);
    CAPTURE(identity_change);
    const auto previous_select_id = region_id("###select_region");
    if (identity_change == 1) {
      view.selected_source_id = SourceId{"another-display"};
    } else if (identity_change == 2) {
      ++view.session_id.value;
    } else if (identity_change == 3) {
      view.content_extent = avb::core::Extent{400, 200};
    }
    if (identity_change != 0) {
      CHECK(region_id("###select_region") != previous_select_id);
      ImGui::ActivateItemByID(previous_select_id);
      draw();
      CHECK(commands.empty());
    }
    const auto select_id = region_id("###select_region");
    view.effective_language = avb::i18n::Language::Japanese;
    draw();
    CHECK(region_id("###select_region") == select_id);
    ImGui::ActivateItemByID(region_id("###select_region"));
    draw();
    REQUIRE(commands.size() == 1);
    const auto *const select = std::get_if<avb::app::SetRegionSelection>(&commands.back());
    REQUIRE(select != nullptr);
    CHECK(select->session_id == view.session_id);
    CHECK(select->source_id == *view.selected_source_id);
    CHECK(select->active);
    commands.clear();
    view.settings.selecting_region = true;
    view.controls.cancel_region_selection.enabled = true;
    draw();
    ImGui::ActivateItemByID(region_id("###cancel_region"));
    draw();
    REQUIRE(commands.size() == 1);
    const auto *const cancel = std::get_if<avb::app::SetRegionSelection>(&commands.back());
    REQUIRE(cancel != nullptr);
    CHECK(cancel->session_id == view.session_id);
    CHECK(cancel->source_id == *view.selected_source_id);
    CHECK_FALSE(cancel->active);
    commands.clear();
    ImGui::ActivateItemByID(region_id("###reset_region"));
    draw();
    REQUIRE(commands.size() == 1);
    const auto *const reset = std::get_if<avb::app::SetRegion>(&commands.back());
    REQUIRE(reset != nullptr);
    CHECK(reset->session_id == view.session_id);
    CHECK(reset->source_id == *view.selected_source_id);
    CHECK_FALSE(reset->region);

    // Content may disappear while selection is active. Cancellation remains
    // available with its original unscoped ID and the current session/source.
    commands.clear();
    view.content_extent.reset();
    view.controls.region.enabled = false;
    draw();
    ImGui::ActivateItemByID(content->GetID("###cancel_region"));
    draw();
    REQUIRE(commands.size() == 1);
    const auto *const missing_content_cancel =
      std::get_if<avb::app::SetRegionSelection>(&commands.back());
    REQUIRE(missing_content_cancel != nullptr);
    CHECK(missing_content_cancel->session_id == view.session_id);
    CHECK(missing_content_cancel->source_id == *view.selected_source_id);
    CHECK_FALSE(missing_content_cancel->active);
  }

  SECTION("controller root fills the client after resizing")
  {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{
      ImGui::CreateContext(), &ImGui::DestroyContext};
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    avb::ui::ControllerView controller;
    const auto view = project(ApplicationState{});
    for (const auto size : {ImVec2{520.0F, 720.0F}, ImVec2{640.0F, 480.0F}}) {
      io.DisplaySize = size;
      ImGui::NewFrame();
      controller.render(view, {});
      auto *const root = ImGui::FindWindowByName("avb0795 Controller");
      REQUIRE(root != nullptr);
      CHECK(root->Pos.x == ImGui::GetMainViewport()->Pos.x);
      CHECK(root->Pos.y == ImGui::GetMainViewport()->Pos.y);
      CHECK(root->Size.x == size.x);
      CHECK(root->Size.y == size.y);
      CHECK((root->Flags & ImGuiWindowFlags_NoTitleBar) != 0);
      ImGui::Render();
    }
  }


  SECTION("controller title actions and scrolling preserve client input")
  {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{
      ImGui::CreateContext(), &ImGui::DestroyContext};
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {640.0F, 480.0F};
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    avb::ui::ControllerView controller;
    const auto view = project(ApplicationState{});
    unsigned quit_count{};
    const auto draw = [&] {
      ImGui::NewFrame();
      const auto frame = controller.render(view, [&](auto command) {
        if (std::holds_alternative<avb::app::Quit>(command)) {
          ++quit_count;
        }
      }, 16.0F);
      ImGui::Render();
      return frame;
    };
    const auto initial = draw();
    const float button_width = ImGui::GetFrameHeight();
    REQUIRE(initial.drag_right > 0.0F);
    REQUIRE(initial.drag_bottom > 16.0F);
    CHECK(initial.drag_right <= io.DisplaySize.x - 16.0F - 2.0F * button_width);
    CHECK_FALSE(initial.minimize_requested);

    // Feed distinct hover/press/release frames through ImGui's real input queue.
    io.AddMousePosEvent(initial.drag_right + button_width * 0.5F,
                        16.0F + button_width * 0.5F);
    draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    const auto pressed = draw();
    CHECK(pressed.drag_right == 0.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    CHECK(draw().minimize_requested);
    CHECK_FALSE(draw().minimize_requested);
    CHECK(quit_count == 0);

    io.AddMousePosEvent(initial.drag_right + button_width * 1.5F +
                         ImGui::GetStyle().ItemSpacing.x,
                        16.0F + button_width * 0.5F);
    draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    draw();
    CHECK(quit_count == 1);
    draw();
    CHECK(quit_count == 1);

    io.AddMousePosEvent(-100.0F, -100.0F);
    io.DisplaySize = {640.0F, 240.0F};
    const auto before_scroll = draw();
    auto *const root = ImGui::FindWindowByName("avb0795 Controller");
    REQUIRE(root != nullptr);
    REQUIRE(root->DC.ChildWindows.Size == 1);
    auto *const child = root->DC.ChildWindows[0];
    REQUIRE(child->ScrollMax.y > 0.0F);
    ImGui::SetScrollY(child, child->ScrollMax.y);
    const auto after_scroll = draw();
    CHECK(child->Scroll.y > 0.0F);
    CHECK(root->Scroll.y == 0.0F);
    CHECK(after_scroll.drag_right == before_scroll.drag_right);
    CHECK(after_scroll.drag_bottom == before_scroll.drag_bottom);

    ImGui::NewFrame();
    ImGui::OpenPopup("controller_test_popup");
    if (ImGui::BeginPopup("controller_test_popup")) {
      ImGui::TextUnformatted("Controlled popup");
      ImGui::EndPopup();
    }
    const auto popup = controller.render(view, {}, 16.0F);
    CHECK(popup.drag_right == 0.0F);
    CHECK(popup.drag_bottom == 0.0F);
    ImGui::Render();
  }

  SECTION("title glyphs share the title background and retain themed feedback")
  {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{
      ImGui::CreateContext(), &ImGui::DestroyContext};
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {640.0F, 480.0F};
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto &style = ImGui::GetStyle();
    // Give line vertices white-atlas UVs instead of the optional AA line texture.
    style.AntiAliasedLinesUseTex = false;
    style.Colors[ImGuiCol_Button] = {0.91F, 0.12F, 0.33F, 1.0F};
    style.Colors[ImGuiCol_ButtonHovered] = {0.16F, 0.87F, 0.41F, 1.0F};
    style.Colors[ImGuiCol_ButtonActive] = {0.24F, 0.38F, 0.94F, 1.0F};
    style.Colors[ImGuiCol_Border] = {0.83F, 0.72F, 0.13F, 1.0F};
    style.FrameBorderSize = 2.0F;
    const auto idle_color = ImGui::GetColorU32(ImGuiCol_Button);
    const auto hover_color = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
    const auto held_color = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    const auto border_color = ImGui::GetColorU32(ImGuiCol_Border);
    avb::ui::ControllerView controller;
    const auto view = project(ApplicationState{});
    const auto draw = [&] {
      ImGui::NewFrame();
      const auto frame = controller.render(view, {}, 16.0F);
      ImGui::Render();
      return frame;
    };
    const auto initial = draw();
    const float button_width = ImGui::GetFrameHeight();
    const auto *const root = ImGui::FindWindowByName("avb0795 Controller");
    REQUIRE(root != nullptr);
    for (int index = 0; index < 2; ++index) {
      CAPTURE(index);
      const float left = initial.drag_right +
        static_cast<float>(index) * (button_width + style.ItemSpacing.x);
      // Distinct colors isolate control backgrounds from the root and child UI.
      // White-atlas UVs identify vector geometry without a raster snapshot.
      const auto has_vertex = [&](ImU32 color, bool solid = false) {
        for (const auto &vertex : root->DrawList->VtxBuffer) {
          if (vertex.pos.x >= left && vertex.pos.x <= left + button_width &&
              vertex.pos.y >= 16.0F && vertex.pos.y <= 16.0F + button_width &&
              vertex.col == color &&
              (!solid || (vertex.uv.x == io.Fonts->TexUvWhitePixel.x &&
                          vertex.uv.y == io.Fonts->TexUvWhitePixel.y))) {
            return true;
          }
        }
        return false;
      };
      io.AddMousePosEvent(-100.0F, -100.0F);
      draw();
      CHECK_FALSE(has_vertex(idle_color));
      CHECK_FALSE(has_vertex(border_color));
      CHECK(has_vertex(ImGui::GetColorU32(ImGuiCol_Text), true));
      CHECK(ImGui::GetColorU32(ImGuiCol_Button) == idle_color);
      CHECK(style.FrameBorderSize == 2.0F);

      io.AddMousePosEvent(left + button_width * 0.5F,
                          16.0F + button_width * 0.5F);
      draw();
      CHECK(has_vertex(hover_color));
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      draw();
      CHECK(has_vertex(held_color));
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      draw();
    }
  }

  SECTION("catalog entries are complete and preserve UTF-8 encoding")
  {
    using namespace avb::i18n;

    constexpr std::array languages{
      Language::English,
      Language::TraditionalChinese,
      Language::Japanese,
    };
    for (const auto language : languages) {
      for (std::size_t index = 0; index < text_count; ++index) {
        const auto value = text(static_cast<TextId>(index), language);
        CAPTURE(language, index);
        CHECK_FALSE(value.empty());
        CHECK(value.find('\0') == std::string_view::npos);
      }
    }

    CHECK(text(TextId::LanguageTraditionalChinese, Language::English) ==
          "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87");
    CHECK(text(TextId::LanguageJapanese, Language::English) ==
          "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
    CHECK(text(TextId::Start, Language::TraditionalChinese) ==
          "\xE9\x96\x8B\xE5\xA7\x8B");
    CHECK(text(TextId::Start, Language::Japanese) ==
          "\xE9\x96\x8B\xE5\xA7\x8B");

    constexpr auto invalid_language = static_cast<Language>(999);
    constexpr auto invalid_text = static_cast<TextId>(999);
    CHECK(text(TextId::Start, invalid_language) == "Start");
    CHECK(text(invalid_text, Language::Japanese) == "Unknown text");
  }

  SECTION("language policy uses supported families and canonical preference tokens")
  {
    using namespace avb::i18n;

    CHECK(match_language_tag("ZH-Hant-TW") == Language::TraditionalChinese);
    CHECK(match_language_tag("ja-JP") == Language::Japanese);
    CHECK(match_language_tag("en") == Language::English);
    CHECK(match_language_tag("en-a1B2c3D4") == Language::English);
    for (const auto tag : {"english", "ja-", "zh--TW", "fr-FR", "",
                           "en-123456789", "en-US_"}) {
      CHECK_FALSE(match_language_tag(tag).has_value());
    }

    struct PreferenceToken {
      LanguagePreference preference;
      std::string_view token;
    };
    constexpr std::array preferences{
      PreferenceToken{LanguagePreference::System, "system"},
      PreferenceToken{LanguagePreference::English, "en"},
      PreferenceToken{LanguagePreference::TraditionalChinese, "zh-TW"},
      PreferenceToken{LanguagePreference::Japanese, "ja"},
    };
    for (const auto &row : preferences) {
      CAPTURE(row.token);
      CHECK(language_preference_token(row.preference) == row.token);
      CHECK(parse_language_preference(row.token) == row.preference);
    }
    CHECK_FALSE(parse_language_preference("ZH-TW").has_value());

    CHECK(resolve_language(LanguagePreference::English, Language::Japanese) ==
          Language::English);
    CHECK(resolve_language(LanguagePreference::TraditionalChinese,
                           Language::Japanese) ==
          Language::TraditionalChinese);
    CHECK(resolve_language(LanguagePreference::Japanese, Language::English) ==
          Language::Japanese);
    CHECK(resolve_language(LanguagePreference::System,
                           Language::TraditionalChinese) ==
          Language::TraditionalChinese);

    constexpr auto invalid_preference =
      static_cast<LanguagePreference>(255);
    constexpr auto invalid_language = static_cast<Language>(255);
    CHECK(language_preference_token(invalid_preference).empty());
    CHECK(resolve_language(invalid_preference, Language::Japanese) ==
          Language::English);
    CHECK(resolve_language(LanguagePreference::System, invalid_language) ==
          Language::English);

    const std::array<std::string_view, 3> preferred{"fr-FR", "ja-JP",
                                                    "en-US"};
    auto chosen = Language::English;
    for (const auto tag : preferred) {
      if (const auto language = match_language_tag(tag)) {
        chosen = *language;
        break;
      }
    }
    CHECK(chosen == Language::Japanese);
  }

#if defined(_WIN32)
  SECTION("installed controller font bakes the catalog without dynamic uploads")
  {
    auto data = avb::platform::windows::load_controller_font();
    REQUIRE(data.has_value());
    ImFontAtlas atlas;
    REQUIRE(avb::desktop::configure_controller_font(atlas, *data).has_value());
    unsigned char *pixels{};
    int width{}, height{};
    atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
    INFO("Static controller atlas: " << width << 'x' << height);
    REQUIRE(pixels != nullptr);
    CHECK(width <= 4096);
    CHECK(height <= 4096);
    auto *const font = atlas.Fonts[0];
    auto *const baked = font->GetFontBaked(font->LegacySize);
    namespace i18n = avb::i18n;
    for (const auto language : {i18n::Language::English,
                                i18n::Language::TraditionalChinese,
                                i18n::Language::Japanese}) {
      for (std::size_t index{}; index < i18n::text_count; ++index) {
        const auto value =
          i18n::text(static_cast<i18n::TextId>(index), language);
        const char *cursor = value.data();
        const char *const end = cursor + value.size();
        while (cursor != end) {
          unsigned int codepoint{};
          const int consumed = ImTextCharFromUtf8(&codepoint, cursor, end);
          REQUIRE(consumed > 0);
          REQUIRE(codepoint != IM_UNICODE_CODEPOINT_INVALID);
          REQUIRE(codepoint <= IM_UNICODE_CODEPOINT_MAX);
          cursor += consumed;

          if (codepoint < 0x20) {
            continue;
          }

          CAPTURE(index, codepoint);
          CHECK(font->IsGlyphInFont(static_cast<ImWchar>(codepoint)));
          REQUIRE(baked->IsGlyphLoaded(static_cast<ImWchar>(codepoint)));
          CHECK(baked->FindGlyphNoFallback(static_cast<ImWchar>(codepoint)) !=
                nullptr);
        }
      }
    }
  }
#endif

  SECTION("localized controller rendering preserves literal source titles and stable IDs")
  {
    using avb::i18n::Language;
    using avb::i18n::LanguagePreference;
    using avb::i18n::text;
    using avb::i18n::TextId;

    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{
      ImGui::CreateContext(), &ImGui::DestroyContext};
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1200.0F, 1600.0F};
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    std::array sources{
      source("budget", SourceKind::Window, ColorSupport::Sdr),
      source("status", SourceKind::Window, ColorSupport::Sdr),
    };
    sources[0].display_label = "Budget##2026";
    sources[1].display_label = "##Status###literal";
    avb::ui::ControllerView controller;

    struct LanguageRow {
      Language language;
      TextId endonym;
    };
    constexpr std::array language_rows{
      LanguageRow{Language::English, TextId::LanguageEnglish},
      LanguageRow{Language::TraditionalChinese,
                  TextId::LanguageTraditionalChinese},
      LanguageRow{Language::Japanese, TextId::LanguageJapanese},
    };

    for (const auto &row : language_rows) {
      CAPTURE(row.language);
      avb::app::UserConfig config;
      config.language = LanguagePreference::System;
      const auto view = project(ApplicationState{}, sources, all_capabilities,
                                CaptureExclusionStatus::Excluded, {}, false,
                                false, std::nullopt, true, config, row.language);

      ImGui::NewFrame();
      ImGui::SetNextWindowSize(io.DisplaySize);
      ImGui::LogToBuffer();
      controller.render(view, {});
      const std::string rendered{context->LogBuffer.c_str()};
      ImGui::LogFinish();

      CHECK(rendered.find(text(TextId::GeneralSettings, row.language)) !=
            std::string::npos);
      CHECK(rendered.find(text(TextId::LanguageLabel, row.language)) !=
            std::string::npos);
      CHECK(rendered.find(text(TextId::FollowSystem, row.language)) !=
            std::string::npos);
      CHECK(rendered.find(text(TextId::EffectiveLanguage, row.language)) !=
            std::string::npos);
      CHECK(rendered.find(text(row.endonym, row.language)) !=
            std::string::npos);
      CHECK(rendered.find(text(TextId::Start, row.language)) !=
            std::string::npos);
      CHECK(rendered.find(text(TextId::CurrentHotkey, row.language)) !=
            std::string::npos);
      CHECK(rendered.find(text(TextId::KeepAlive, row.language)) !=
            std::string::npos);
      CHECK(rendered.find(text(TextId::ExtensionsScope, row.language)) !=
            std::string::npos);
      CHECK(rendered.find(text(TextId::SourceWindow, row.language)) !=
            std::string::npos);
      CHECK(rendered.find(sources[0].display_label) != std::string::npos);
      CHECK(rendered.find(sources[1].display_label) != std::string::npos);
      CHECK(rendered.find("(?)") != std::string::npos);

      auto *const window = ImGui::FindWindowByName("avb0795 Controller");
      REQUIRE(window != nullptr);
      const auto window_label =
        std::string{text(TextId::ControllerTitle, row.language)} +
        "###avb0795 Controller";
      CHECK(ImHashStr(window_label.c_str()) == window->ID);
      CHECK(window->ID == ImHashStr("avb0795 Controller"));
      const auto start_label =
        std::string{text(TextId::Start, row.language)} + "###start";
      const auto hotkey_label =
        std::string{text(TextId::RecordHotkey, row.language)} +
        "###record_hotkey";
      const auto selector_label =
        std::string{text(TextId::LanguageLabel, row.language)} +
        "###language";
      CHECK(window->GetID(start_label.c_str()) == window->GetID("start"));
      CHECK(window->GetID(hotkey_label.c_str()) ==
            window->GetID("record_hotkey"));
      CHECK(window->GetID(selector_label.c_str()) ==
            window->GetID("language"));
      ImGui::EndFrame();
    }
  }

  SECTION("source rows and the current session are portable values")
  {
    const std::array sources{
      source("display", SourceKind::Display, ColorSupport::Sdr),
      source("window", SourceKind::Window, ColorSupport::Sdr),
    };
    ApplicationState state;
    const auto session_id = state.current_session_id();
    state.select_source(session_id, sources[1]).value();
    REQUIRE(state.set_opacity(session_id, 40));
    REQUIRE_FALSE(state.set_opacity(avb::core::SessionId{session_id.value + 1}, 20));

    const auto view = project(state, sources);

    REQUIRE(view.sources.size() == 2);
    CHECK(view.session_id == session_id);
    CHECK(view.settings.opacity_percent == 40);
    CHECK(view.sources[0].id == sources[0].id);
    CHECK(view.sources[0].kind_glyph == "display");
    CHECK(view.sources[0].label == "Display 1");
    CHECK_FALSE(view.sources[0].selected);
    CHECK(view.sources[1].kind_glyph == "window");
    CHECK(view.sources[1].label == "Editor");
    CHECK(view.sources[1].selected);
    CHECK(view.hotkey_display_text == "Ctrl+Alt+R");
    CHECK(view.hotkey_binding_confirmed);

    const avb::core::SessionId projected_session_id{42};
    const auto explicitly_projected = avb::app::build_controller_view(
      state, projected_session_id, sources, all_capabilities,
      avb::app::UserConfig{}, true, CaptureExclusionStatus::Excluded,
      {}, false, false);
    CHECK(explicitly_projected.session_id == projected_session_id);

    STATIC_CHECK_FALSE(has_thumbnail<ControllerViewState>);
    STATIC_CHECK_FALSE(has_pixels<ControllerViewState>);
    STATIC_CHECK_FALSE(has_pixel_data<ControllerViewState>);
    STATIC_CHECK_FALSE(
      has_thumbnail<avb::app::ControllerSourceRow>);
    STATIC_CHECK_FALSE(has_pixels<avb::app::ControllerSourceRow>);
    STATIC_CHECK_FALSE(has_pixel_data<avb::app::ControllerSourceRow>);
  }

  SECTION("notice bridge reason and drain status remain distinct diagnostics")
  {
    ApplicationState state;
    RuntimeFeedback feedback{
      .overlay_exclusion = CaptureExclusionStatus::Excluded,
      .bridge = BridgeFeedback{
        .mode = BridgeMode::CpuCopy,
        .reason = BridgeReason::InteropOperationFailure,
      },
      .drain = RuntimeDrainStatus::Normal,
    };

    const auto normal = project(
      state, {}, all_capabilities, CaptureExclusionStatus::Excluded, feedback,
      true, false, AppError{AppErrorCode::ConfigWriteFailure}, false);

    REQUIRE(normal.application_notice_text.has_value());
    CHECK(*normal.application_notice_text ==
          avb::app::error_text(AppErrorCode::ConfigWriteFailure,
                                     avb::i18n::Language::English));
    REQUIRE(normal.bridge.has_value());
    CHECK(normal.bridge->mode == BridgeMode::CpuCopy);
    CHECK(normal.bridge->reason == BridgeReason::InteropOperationFailure);
    CHECK_FALSE(normal.bridge->status_text.empty());
    CHECK(normal.drain_status == RuntimeDrainStatus::Normal);
    CHECK_FALSE(normal.drain_warning.has_value());
    CHECK_FALSE(normal.hotkey_binding_confirmed);

    feedback.drain = RuntimeDrainStatus::Stalled;
    const auto stalled = project(state, {}, all_capabilities,
                                 CaptureExclusionStatus::Excluded, feedback,
                                 true);
    CHECK(stalled.drain_status == RuntimeDrainStatus::Stalled);
    REQUIRE(stalled.drain_warning.has_value());
    CHECK_FALSE(stalled.drain_warning->empty());

    const auto absent_runtime = project(
      state, {}, all_capabilities, CaptureExclusionStatus::Excluded, feedback);
    CHECK_FALSE(absent_runtime.bridge.has_value());
    CHECK(absent_runtime.drain_status == RuntimeDrainStatus::Normal);
    CHECK_FALSE(absent_runtime.drain_warning.has_value());
  }

  SECTION("color and controller or overlay exclusion warnings are independent")
  {
    ApplicationState state;
    const auto session_id = state.current_session_id();

    std::array color_sources{
      source("sdr", SourceKind::Display, ColorSupport::Sdr),
      source("hdr", SourceKind::Display,
             ColorSupport::HdrDetectedUnsupported),
      source("unknown", SourceKind::Display,
             ColorSupport::UnknownAssumeSdr),
    };

    state.select_source(session_id, color_sources[0]).value();
    CHECK_FALSE(project(state, color_sources).color_warning.has_value());

    state.select_source(session_id, color_sources[1]).value();
    const auto hdr = project(state, color_sources);
    REQUIRE(hdr.color_warning.has_value());
    CHECK(*hdr.color_warning == "HDR was detected but is unsupported.");

    state.select_source(session_id, color_sources[2]).value();
    const auto unknown = project(state, color_sources);
    REQUIRE(unknown.color_warning.has_value());
    CHECK(*unknown.color_warning ==
          "Color detection is unavailable; SDR is assumed.");

    RuntimeFeedback overlay_failed{
      .overlay_exclusion = CaptureExclusionStatus::Failed};
    const auto first = project(
      state, color_sources, all_capabilities,
      CaptureExclusionStatus::Unsupported, overlay_failed, true);
    REQUIRE(first.controller_exclusion_warning.has_value());
    REQUIRE(first.overlay_exclusion_warning.has_value());
    CHECK(*first.controller_exclusion_warning !=
          *first.overlay_exclusion_warning);

    overlay_failed.overlay_exclusion = CaptureExclusionStatus::Unsupported;
    const auto second = project(state, color_sources, all_capabilities,
                                CaptureExclusionStatus::Failed, overlay_failed,
                                true);
    REQUIRE(second.controller_exclusion_warning.has_value());
    REQUIRE(second.overlay_exclusion_warning.has_value());
    CHECK(*first.controller_exclusion_warning !=
          *second.controller_exclusion_warning);
    CHECK(*first.overlay_exclusion_warning !=
          *second.overlay_exclusion_warning);
  }

  SECTION("capabilities disable only their controls with explanations")
  {
    const std::array sources{
      source("display", SourceKind::Display, ColorSupport::Sdr),
      source("window", SourceKind::Window, ColorSupport::Sdr),
    };
    ApplicationState state;
    state.select_source(state.current_session_id(), sources[0]).value();
    const PlatformCapabilities unavailable{};

    const auto view = project(state, sources, unavailable);

    CHECK_FALSE(view.sources[0].select.enabled);
    CHECK_FALSE(view.sources[0].select.explanation.empty());
    CHECK_FALSE(view.sources[1].select.enabled);
    CHECK_FALSE(view.controls.start.enabled);
    CHECK(view.controls.start.explanation ==
          "This capability is unavailable.");
    CHECK(view.controls.editable.enabled);
    CHECK_FALSE(view.controls.pass_through.enabled);
    CHECK(view.controls.fps.enabled);
    CHECK(view.controls.opacity.enabled);
    CHECK(view.controls.bordered.enabled);
    CHECK_FALSE(view.controls.borderless.enabled);
    CHECK_FALSE(view.controls.replace_hotkey.enabled);
    CHECK_FALSE(view.controls.browser_capture_keep_alive.enabled);
    CHECK(view.controls.browser_capture_keep_alive.explanation ==
          "This capability is unavailable.");
    CHECK(view.controls.refresh.enabled);

    const auto idle = project(ApplicationState{});
    CHECK(idle.controls.browser_capture_keep_alive.enabled);
    CHECK(idle.extensions_scope_text == "Window captures only");
  }

  SECTION("lifecycle runtime and precedence produce one command policy")
  {
    struct LifecycleExpectation {
      SessionLifecycle lifecycle;
      bool runtime_present;
      bool stop_requested;
      bool refresh;
      bool select;
      bool start;
      bool stop;
      bool settings;
    };

    constexpr std::array rows{
      LifecycleExpectation{SessionLifecycle::Idle, false, false, true, true,
                           true, false, true},
      LifecycleExpectation{SessionLifecycle::Starting, true, false, false,
                           false, false, false, false},
      LifecycleExpectation{SessionLifecycle::Capturing, true, false, false,
                           false, false, true, true},
      LifecycleExpectation{SessionLifecycle::RecoveringGraphics, true, false,
                           false, false, false, true, true},
      LifecycleExpectation{SessionLifecycle::Interrupted, true, false, false,
                           true, true, true, true},
      LifecycleExpectation{SessionLifecycle::Stopping, true, true, false,
                           false, false, false, false},
      LifecycleExpectation{SessionLifecycle::Error, true, true, false, false,
                           false, false, false},
      LifecycleExpectation{SessionLifecycle::Error, false, false, true, true,
                           true, false, true},
    };
    const std::array sources{
      source("display", SourceKind::Display, ColorSupport::Sdr)};

    for (const auto &row : rows) {
      CAPTURE(row.lifecycle, row.runtime_present, row.stop_requested);
      const auto state = state_in(row.lifecycle);
      const auto view = project(state, sources, all_capabilities,
                                CaptureExclusionStatus::Excluded, {},
                                row.runtime_present, row.stop_requested);
      CHECK(view.controls.refresh.enabled == row.refresh);
      CHECK(view.sources.front().select.enabled == row.select);
      CHECK(view.controls.start.enabled == row.start);
      CHECK(view.controls.stop.enabled == row.stop);
      CHECK(view.controls.editable.enabled == row.settings);
      CHECK(view.controls.fps.enabled == row.settings);
      CHECK(view.controls.opacity.enabled == row.settings);
      CHECK(view.controls.bordered.enabled == row.settings);
      CHECK(view.controls.browser_capture_keep_alive.enabled == row.settings);
      CHECK(view.controls.language.enabled);
    }

    const auto error_state = state_in(SessionLifecycle::Error);
    const auto error_view = project(error_state, sources);
    REQUIRE(error_view.session_error_text.has_value());
    CHECK(*error_view.session_error_text == "Replica session failed.");

    ApplicationState missing_source;
    CHECK(project(missing_source).controls.start.explanation ==
          "Select a source first.");

    const PlatformCapabilities unavailable{};
    const auto starting_state = state_in(SessionLifecycle::Starting);
    const auto lifecycle_first = project(starting_state, sources, unavailable);
    CHECK(lifecycle_first.controls.start.explanation ==
          "This action is unavailable in the current lifecycle.");

    ApplicationState idle_with_source;
    idle_with_source
      .select_source(idle_with_source.current_session_id(), sources.front())
      .value();
    CHECK(project(idle_with_source, sources, unavailable)
            .controls.start.explanation == "This capability is unavailable.");

    const auto drain_first = project(missing_source, {}, unavailable,
                                     CaptureExclusionStatus::Excluded, {},
                                     true, true);
    CHECK(drain_first.controls.start.explanation ==
          "The current replica is stopping.");
    CHECK(drain_first.controls.browser_capture_keep_alive.explanation ==
          "The current replica is stopping.");

    const RuntimeFeedback stalled_feedback{
      .drain = RuntimeDrainStatus::Stalled};
    const auto recovering = state_in(SessionLifecycle::RecoveringGraphics);
    const auto stalled_recovery = project(
      recovering, sources, all_capabilities,
      CaptureExclusionStatus::Excluded, stalled_feedback, true);
    CHECK(stalled_recovery.controls.stop.enabled);
    REQUIRE(stalled_recovery.drain_warning.has_value());
    CHECK_FALSE(stalled_recovery.drain_warning->empty());

    const auto shutdown_view = project(
      ApplicationState{}, {}, all_capabilities,
      CaptureExclusionStatus::Excluded, {}, false, false, std::nullopt, true,
      {}, avb::i18n::Language::Japanese, false);
    CHECK_FALSE(shutdown_view.controls.language.enabled);
    CHECK(shutdown_view.controls.language.explanation ==
          avb::i18n::text(
            avb::i18n::TextId::LanguageChangesClosed,
            avb::i18n::Language::Japanese));
  }

  SECTION("localized projection selects exact catalog entries in every language")
  {
    using avb::i18n::Language;
    using avb::i18n::LanguagePreference;
    using avb::i18n::text;
    using avb::i18n::TextId;

    struct ErrorTextRow {
      AppErrorCode code;
      TextId text_id;
      std::string_view diagnostic;
    };
    constexpr std::array error_rows{
      ErrorTextRow{AppErrorCode::InvalidCommand, TextId::ErrorInvalidCommand,
                   "invalid_command"},
      ErrorTextRow{AppErrorCode::InvalidValue, TextId::ErrorInvalidValue,
                   "invalid_value"},
      ErrorTextRow{AppErrorCode::ConfigReadFailure,
                   TextId::ErrorConfigReadFailure, "config_read_failure"},
      ErrorTextRow{AppErrorCode::ConfigWriteFailure,
                   TextId::ErrorConfigWriteFailure, "config_write_failure"},
      ErrorTextRow{AppErrorCode::SourceCatalogFailure,
                   TextId::ErrorSourceCatalogFailure,
                   "source_catalog_failure"},
      ErrorTextRow{AppErrorCode::PermissionDenied,
                   TextId::ErrorPermissionDenied, "permission_denied"},
      ErrorTextRow{AppErrorCode::SourceUnavailable,
                   TextId::ErrorSourceUnavailable, "source_unavailable"},
      ErrorTextRow{AppErrorCode::HotkeyConflict, TextId::ErrorHotkeyConflict,
                   "hotkey_conflict"},
      ErrorTextRow{AppErrorCode::HotkeyCleanupFailure,
                   TextId::ErrorHotkeyCleanupFailure,
                   "hotkey_cleanup_failure"},
      ErrorTextRow{AppErrorCode::HotkeyRegistrationLost,
                   TextId::ErrorHotkeyRegistrationLost,
                   "hotkey_registration_lost"},
      ErrorTextRow{AppErrorCode::UnsupportedWindowClass,
                   TextId::ErrorUnsupportedWindowClass,
                   "unsupported_window_class"},
      ErrorTextRow{AppErrorCode::UnsupportedGraphics,
                   TextId::ErrorUnsupportedGraphics, "unsupported_graphics"},
      ErrorTextRow{AppErrorCode::RuntimeConstructionFailure,
                   TextId::ErrorRuntimeConstructionFailure,
                   "runtime_construction_failure"},
      ErrorTextRow{AppErrorCode::PlatformOperationFailure,
                   TextId::ErrorPlatformOperationFailure,
                   "platform_operation_failure"},
      ErrorTextRow{AppErrorCode::RuntimeStateLost,
                   TextId::ErrorRuntimeStateLost, "runtime_state_lost"},
    };
    struct LanguageRow {
      Language language;
      LanguagePreference preference;
    };
    constexpr std::array language_rows{
      LanguageRow{Language::English, LanguagePreference::English},
      LanguageRow{Language::TraditionalChinese,
                  LanguagePreference::TraditionalChinese},
      LanguageRow{Language::Japanese, LanguagePreference::Japanese},
    };

    for (const auto &error_row : error_rows) {
      const auto error_view = project(
        ApplicationState{}, {}, all_capabilities,
        CaptureExclusionStatus::Excluded, {}, false, false,
        AppError{error_row.code});
      REQUIRE(error_view.application_notice_text.has_value());
      CHECK(*error_view.application_notice_text ==
            text(error_row.text_id, Language::English));
      CHECK(AppError{error_row.code}.diagnostic_label() ==
            error_row.diagnostic);
    }

    for (const auto &language_row : language_rows) {
      CAPTURE(language_row.language);
      avb::app::UserConfig config;
      config.language = language_row.preference;

      std::array sources{
        source("display", SourceKind::Display, ColorSupport::Sdr),
        source("window", SourceKind::Window,
               ColorSupport::UnknownAssumeSdr),
      };
      ApplicationState state;
      const auto session_id = state.current_session_id();
      state.select_source(session_id, sources.back()).value();
      state.begin_start(session_id).value();
      state.mark_capturing(session_id).value();
      const RuntimeFeedback feedback{
        .overlay_exclusion = CaptureExclusionStatus::Failed,
        .bridge = BridgeFeedback{
          .mode = BridgeMode::CpuCopy,
          .reason = BridgeReason::InteropOperationFailure,
        },
        .drain = RuntimeDrainStatus::Stalled,
      };
      const auto view = project(
        state, sources, all_capabilities, CaptureExclusionStatus::Unsupported,
        feedback, true, false,
        AppError{AppErrorCode::ConfigWriteFailure}, false, config,
        language_row.language);

      CHECK(view.language_preference == language_row.preference);
      CHECK(view.effective_language == language_row.language);
      CHECK(view.status_text ==
            text(TextId::Capturing, language_row.language));
      REQUIRE(view.application_notice_text.has_value());
      CHECK(*view.application_notice_text ==
            text(TextId::ErrorConfigWriteFailure, language_row.language));
      REQUIRE(view.sources.size() == 2);
      CHECK(view.sources.front().kind_glyph ==
            text(TextId::SourceDisplay, language_row.language));
      CHECK(view.sources.back().kind_glyph ==
            text(TextId::SourceWindow, language_row.language));
      CHECK(view.sources.front().label == "Display 1");
      CHECK(view.sources.back().label == "Editor");
      REQUIRE(view.bridge.has_value());
      CHECK(view.bridge->status_text ==
            text(TextId::BridgeInteropFailure, language_row.language));
      REQUIRE(view.color_warning.has_value());
      CHECK(*view.color_warning ==
            text(TextId::ColorUnknownSdr, language_row.language));
      REQUIRE(view.controller_exclusion_warning.has_value());
      CHECK(*view.controller_exclusion_warning ==
            text(TextId::ControllerExclusionUnsupported,
                 language_row.language));
      REQUIRE(view.overlay_exclusion_warning.has_value());
      CHECK(*view.overlay_exclusion_warning ==
            text(TextId::OverlayExclusionFailed, language_row.language));
      REQUIRE(view.drain_warning.has_value());
      CHECK(*view.drain_warning ==
            text(TextId::DrainStalled, language_row.language));
      CHECK(view.extensions_scope_text ==
            text(TextId::ExtensionsScope, language_row.language));

      const auto session_error = project(
        state_in(SessionLifecycle::Error), {}, all_capabilities,
        CaptureExclusionStatus::Excluded, {}, true, true, std::nullopt, true,
        config, language_row.language);
      REQUIRE(session_error.session_error_text.has_value());
      CHECK(session_error.status_text == text(TextId::Error,
                                              language_row.language));
      CHECK(*session_error.session_error_text ==
            text(TextId::SessionFailed, language_row.language));

      const PlatformCapabilities unavailable{};
      const auto lifecycle_disabled = project(
        state_in(SessionLifecycle::Starting), sources, unavailable,
        CaptureExclusionStatus::Excluded, {}, true, false, std::nullopt, true,
        config, language_row.language);
      CHECK(lifecycle_disabled.controls.start.explanation ==
            text(TextId::ControlLifecycle, language_row.language));

      ApplicationState idle_with_source;
      idle_with_source
        .select_source(idle_with_source.current_session_id(), sources.back())
        .value();
      const auto capability_disabled = project(
        idle_with_source, sources, unavailable,
        CaptureExclusionStatus::Excluded, {}, false, false, std::nullopt, true,
        config, language_row.language);
      CHECK(capability_disabled.controls.start.explanation ==
            text(TextId::ControlCapability, language_row.language));

      const auto missing_source = project(
        ApplicationState{}, {}, all_capabilities,
        CaptureExclusionStatus::Excluded, {}, false, false, std::nullopt, true,
        config, language_row.language);
      CHECK(missing_source.controls.start.explanation ==
            text(TextId::ControlMissingSource, language_row.language));

      const auto stopping = project(
        ApplicationState{}, {}, unavailable, CaptureExclusionStatus::Excluded,
        {}, true, true, std::nullopt, true, config, language_row.language);
      CHECK(stopping.controls.start.explanation ==
            text(TextId::ControlStopping, language_row.language));
    }
  }
}
