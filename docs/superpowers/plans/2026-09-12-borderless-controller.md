# Borderless Controller Implementation Plan

> For agentic workers: use subagent-driven-development for the native adapter and focused fresh reviews. Preserve existing checkout changes and leave tests/spec/plan out of commits.

**Goal:** Present a single full-client ImGui controller with native drag/resize, minimize and orderly close.

**Architecture:** ControllerView returns value-only title geometry/minimize intent. DesktopHost converts geometry to normalized client fractions and owns a narrow interaction port returned by its existing startup callback. The Windows adapter implements that port; main composes it. Existing renderer and application ownership stay in place.

**Tech Stack:** C++23, ImGui 1.92.9, GLFW 3.4, Win32 subclass helpers, Vulkan, Catch2, native clang-cl Release build.

## Task 1: Controller native interaction boundary

Files:
- Add include/airelinna/desktop/controller_window_interaction.hpp: portable lifetime/drag-region port.
- Add platform/windows/windows_controller_window.hpp and .cpp: checked native subclass/frame setup and hit testing.
- Modify src/CMakeLists.txt and cmake/VerifySourceBoundaries.cmake: source owner airelinna_windows, private comctl32, exact controller-port include exception.
- Extend tests/windows/overlay_policy_test.cpp within existing case 11.

- [x] Add a hidden top-level fixture and focused sections before implementing behavior. Exercise actual subclass dispatch:
```cpp
const auto adapter = WindowsControllerWindow::create(window.get());
REQUIRE(adapter.has_value());
(*adapter)->set_drag_region(0.75F, 0.10F);
CHECK(hit_at(100, 20) == HTCAPTION);
CHECK(hit_at(480, 20) == HTCLIENT);
CHECK(hit_at(100, 150) == HTCLIENT);
CHECK(hit_at(1, 1) == HTTOPLEFT);
CHECK(hit_at(width - 1, height - 1) == HTBOTTOMRIGHT);
```
Use coordinates relative to GetClientRect and ClientToScreen; send WM_NCHITTEST with signed screen coordinates. Add the other edges/corners as a compact table. Validate WM_NCCALCSIZE full-client behavior, minimum track dimensions at the host DPI, disabled drag region, invalid handle, and safe detach/WM_NCDESTROY. Keep all native windows hidden; avoid capture sessions.
- [x] Build a minimal interface/stub when necessary for a runtime red result; run the focused case and record the expected failure before replacing the stub.
- [x] Define the port exactly:
```cpp
namespace airelinna::desktop {
class ControllerWindowInteraction {
public:
  virtual ~ControllerWindowInteraction() = default;
  virtual void set_drag_region(float right_fraction, float bottom_fraction) noexcept = 0;
protected:
  ControllerWindowInteraction() = default;
  ControllerWindowInteraction(const ControllerWindowInteraction&) = delete;
  ControllerWindowInteraction& operator=(const ControllerWindowInteraction&) = delete;
};
}
```
Provide high-information Doxygen about normalized top-left client geometry, main-thread use, and owner-before-window destruction.
- [x] Implement WindowsControllerWindow final with create(HWND) returning expected<unique_ptr<WindowsControllerWindow>, core::AppError>, protected callback storage, stable address and deleted copy/move. Main-thread-only subclass registration uses SetWindowSubclass/DefSubclassProc/RemoveWindowSubclass with receiver data.
- [x] Check IsWindow, owning process/thread, SetWindowLongPtrW and SetWindowPos. Save old style, install subclass before SWP_FRAMECHANGED, restore on setup failure. Native sizing uses WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX; strip caption/maximize chrome. WM_NCCALCSIZE returns a full client. WM_NCHITTEST evaluates DPI-scaled edges first, then normalized title rectangle, then HTCLIENT. Clamp fractions and reject nonfinite geometry. Forward messages outside the controller-specific policy.
- [x] WM_GETMINMAXINFO enforces 320 x 240 logical minimum via current DPI; process signed coordinates and suppress caption double-click maximize. Detach on WM_NCDESTROY; destructor removes callback before storage release. Retain DPI forwarding to GLFW. Do not reuse replica aspect/pass-through behavior.
- [x] Add comctl32 solely to airelinna_windows link allowlist. Windows source scan admits desktop/controller_window_interaction.hpp solely from windows_controller_window.hpp/.cpp; UI and DesktopHost native restrictions remain exact.
- [x] Run policy case 11 and source-boundary target. Fresh task review checks native behavior and ownership against spec, followed by quality review; resolve genuine Critical/Medium findings.

## Task 2: Full-client UI and host integration

Files:
- Modify include/airelinna/ui/controller_view.hpp and src/ui/controller_view.cpp.
- Modify include/airelinna/desktop/desktop_host.hpp and src/desktop/desktop_host.cpp.
- Modify main.cpp.
- Modify include/airelinna/i18n/catalog.hpp and src/i18n/catalog.cpp for the minimize tooltip.
- Extend tests/app/controller_contract_test.cpp within existing case 07.

- [x] Add a real ImGui-frame regression section asserting root position equals main viewport position, root size tracks 520 x 720 and 640 x 480, and title remains at the top when content scrolls. Run against old code to establish runtime red.
- [x] Add the presentation result:
```cpp
struct ControllerFrame {
  float drag_right{};
  float drag_bottom{};
  bool minimize_requested{};
};
ControllerFrame ControllerView::render(const app::ControllerViewState&,
                                       const CommandSink&,
                                       float resize_margin = 8.0F);
```
Pixels are client-relative ImGui display coordinates. Host supplies a DPI-scaled resize margin, and the view reserves at least 8 display units. Rendering returns zero drag bounds when a popup/modal is open or an active widget has the left mouse button held. Close emits app::Quit through the current sink. Minimize returns a per-frame boolean.
- [x] Set next root position/size every frame to GetMainViewport()->Pos/Size. Root flags NoTitleBar|NoMove|NoResize|NoCollapse|NoSavedSettings|NoScrollbar|NoScrollWithMouse. Style rounding/border become zero; padding reserves resize margins. Draw a title row and right-aligned minimize/close buttons with stable IDs, then BeginChild for scrollable existing controls. Keep Begin/End and BeginChild/EndChild paired even on clipping.
- [x] Add localized Minimize text in the existing catalog (English, Traditional Chinese, Japanese). Reuse existing Quit text for close tooltip. Keep general-settings content, existing command IDs and existing source literal handling.
- [x] Extend the startup return:
```cpp
struct ControllerWindowSetup {
  core::CaptureExclusionStatus exclusion{core::CaptureExclusionStatus::NotAttempted};
  std::unique_ptr<ControllerWindowInteraction> interaction;
};
using ControllerWindowCreated =
  std::function<std::expected<ControllerWindowSetup, core::AppError>(GLFWwindow*)>;
```
main first calls prepare_controller_window, then WindowsControllerWindow::create, then returns both results. Propagate either error unchanged.
- [x] Move the interaction owner into Impl before graphics initialization. The exact cleanup order is ImGui backend shutdown via cleanup_partial(), interaction reset, then glfwDestroyWindow. Constructor exception handling shuts down initialized backends before member unwinding releases the adapter and the outer create catches/destroys the native window.
- [x] DesktopHost explicitly sets GLFW_DECORATED=false, GLFW_VISIBLE=false, GLFW_FLOATING=false for controller creation and applies the adapter before showing the completed host. Reset GLFW_VISIBLE hint after creation so replica creation retains its current visibility contract.
- [x] After each UI render, compute fractions from ImGui DisplaySize and call the port. Zero display extent produces zero fractions. Process minimize through glfwIconifyWindow. Existing Quit queue, application.step, swapchain recreation, controller GPU retirement and abandon behavior remain unchanged.
- [x] Add precise action tests by feeding ImGui mouse events through press/release frames; assert one minimize result and existing Quit variant delivery. Reuse existing catalog and command tests. Check popup/active-widget drag suppression and child input. Existing test count stays twelve.
- [x] Run core case 07 and all non-hardware tests. Fresh reviewers validate spec compliance and then code quality. Update test expectations only where the intended child layout changes internal window identity.

## Task 3: Documentation and delivery

Files:
- Modify docs/architecture/desktop-host.md, docs/architecture/application-ui.md, docs/architecture/windows-runtime-overlay.md and docs/architecture/README.md.
- Update README.md usage section and local docs/verification/windows-v1-checklist.md.

- [x] Document full-client UI, frame result, normalized geometry, native ownership, shutdown routing, and new file inventory in existing Chinese architecture pages. Add compact Mermaid edges where current diagrams need the new seam.
- [x] Build and verify from the VS x64 developer environment:
```text
cmd /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build build/p4-release --target Airelinna airelinna_core_tests airelinna_windows_policy_tests verify_source_boundaries'
ctest --test-dir build/p4-release --output-on-failure -LE hardware
```
Expected: all targets exit 0, 12/12 CTest cases pass, layer scan passes.
- [x] Final fresh complete scoped review checks all changed source/headers/build rules/tests/docs against current checkout. Validate and fix genuine Critical/Medium issues, then repeat with a new reviewer until clean.
- [x] Run scoped diff/format checks and document exact test evidence. Deliver build/p4-release/src/Airelinna.exe and manual checklist: drag title, resize edges/corners, click/edit/scroll, minimize/restore, Alt+F4 and X during capture, screenshots and mixed-DPI movement.
- [x] Preserve preexisting edits and local test/spec/plan status. No unrelated repository cleanup or external pushes.
