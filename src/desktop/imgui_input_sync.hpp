/// @file
/// @brief Event-driven input synchronization for a hidden controller window.
#ifndef AVB0795_DESKTOP_IMGUI_INPUT_SYNC_HPP
#define AVB0795_DESKTOP_IMGUI_INPUT_SYNC_HPP
#include <imgui.h>
#include <utility>
namespace avb::desktop::detail {

  /// @brief Consume the complete pending input batch and preserve visible-frame policy.
  ///
  /// A minimized host can sleep until another event arrives, so a release or focus
  /// change queued behind a press needs to reach ImGui in this same frame. Temporarily
  /// disabling input trickling lets NewFrame consume that batch; EndFrame completes
  /// the synchronization. The caller supplies backend NewFrame operations, while
  /// widget rendering, command generation and native presentation use the visible path.
  /// A local guard restores the caller's trickling preference on return or exception.
  /// @pre The host thread owns a live ImGui context and is outside an active frame.
  template<class BackendNewFrame>
  void synchronize_hidden_imgui_input(BackendNewFrame&& backend_new_frame)
  {
    auto& io = ImGui::GetIO();
    struct RestoreTrickling final {
      ImGuiIO& io;
      bool previous;
      ~RestoreTrickling() noexcept { io.ConfigInputTrickleEventQueue = previous; }
    } restore{io, io.ConfigInputTrickleEventQueue};
    io.ConfigInputTrickleEventQueue = false;
    std::forward<BackendNewFrame>(backend_new_frame)();
    ImGui::NewFrame();
    ImGui::EndFrame();
  }

} // namespace avb::desktop::detail
#endif // AVB0795_DESKTOP_IMGUI_INPUT_SYNC_HPP
