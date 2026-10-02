# Borderless ImGui Controller Design

## Approved outcome

Airelinna starts with one visible controller whose client area is filled by ImGui. A custom title row supplies a drag area, minimize button and close button. The four edges and corners resize the native controller. The existing replica remains a separately owned capture presentation window. Default screenshot visibility and current-process source filtering remain intact.

## Responsibility and data flow

1. ControllerView projects application state, draws a fixed full-client root, and returns a small ControllerFrame value containing title drag bounds in ImGui display units and a minimize request. Its close button emits the existing app::Quit through the existing command sink.
2. DesktopHost consumes this value after rendering, normalizes the drag region against ImGui display dimensions, updates a narrowly scoped ControllerWindowInteraction port, and calls its existing GLFW backend for minimize. Application continues owning Quit admission, capture draining and shutdown disposition.
3. A WindowsControllerWindow adapter implements the port with native hit testing. It borrows the controller window and owns a subclass registration; it handles native frame layout, edges/corners, title hit regions and minimum track size. It forwards ordinary input and lifecycle messages through the subclass chain.
4. main.cpp composes controller validation and WindowsControllerWindow construction in the existing controller-created callback. ControllerWindowSetup returns the exclusion status and unique interaction owner. DesktopHost owns that adapter throughout its native window lifetime and detaches it before destroying the window.

The portable port resides at include/airelinna/desktop/controller_window_interaction.hpp and exposes a virtual destructor and set_drag_region(float right_fraction, float bottom_fraction) noexcept. Fractions describe a top-left anchored client rectangle; zero values disable caption dragging. The UI uses its own presentation value and retains its existing dependency direction. The Windows adapter receives a narrow allowlist exception for this exact port header; all other desktop/UI implementation includes retain their existing boundary checks.

## Interaction contract

- Root position and dimensions follow the ImGui main viewport every frame; zero rounding and zero outer border make the client completely covered.
- Title row remains fixed while a child content region scrolls. The resize band stays clear of title buttons and content controls.
- Only the title rectangle to the left of the minimize/close buttons yields caption dragging. Content, popups, buttons and text editing retain normal client input.
- Edge/corner hit testing takes priority. Resize widths use current window DPI and are clamped for small extents. Client screen coordinates are obtained with signed coordinate extraction, supporting monitors left/above the primary display.
- Initial size stays 520 x 720. The controller has a DPI-aware minimum size adequate for the two title buttons and scrolling content (320 x 240 logical pixels).
- Native title double-click is consumed; maximize is outside this feature. Minimize/restore and Alt+F4 retain their usual behavior.
- The native adapter preserves GLFW callback storage and forwarding, including size, DPI, keyboard and close delivery.
- Frame styles provide native sizing/system-menu/minimize behavior while WM_NCCALCSIZE extends client rendering across the frame. Decorative Windows chrome is replaced by the custom title row.
- Host creation configures native style before showing the controller. Popup UI remains inside the existing main viewport. Drag-region data is suppressed while an ImGui popup/modal or active widget requires client input.

## Ownership and failures

The adapter is single-threaded on the window-owning thread. Construction validates window identity/thread, installs the subclass and applies frame changes transactionally. Failure returns RuntimeConstructionFailure with native detail after restoring changed state; a failed restoration returns RuntimeStateLost under the existing native rollback policy. Destruction removes the subclass while its receiver is alive. WM_NCDESTROY clears the borrowed handle. Both normal cleanup and constructor unwinding shut down initialized ImGui backends before detaching the adapter, then destroy the native window. Existing GPU retirement and abandon-process behavior remain unchanged.

The existing overlay adapter continues handling replica-specific aspect preservation and pass-through. This feature adds a controller-specific adapter with bounded behavior. Capture services, renderer, config format, language selection, model/domain state, and multi-viewports remain under their existing owners and contracts.

## Verification scope

Reuse the existing two test executables and cases 07/11. Add focused sections for full-client root layout/title actions and native controller hit testing/lifetime. Native fixtures own hidden windows and generate messages locally; visual capture and external user-window interaction are outside automated checks. Cover title vs buttons/content, four edges/corners, signed screen coordinates, high DPI/minimum sizes, detach/invalid handles, root layout after resizing/scrolling, and Quit emission.

Run the native Release product/test build, existing 12-case non-hardware CTest suite, and verify_source_boundaries. Manual acceptance covers actual mouse drag/resize, minimize/taskbar restore, mixed-DPI monitors, scrolling/popup interaction, screenshot visibility and capture-active close/drain. Record automated evidence separately from manual acceptance.

## Work discipline

Preserve the current dirty checkout and existing screenshot changes. Keep tests, spec, plan and verification records local. Use fresh reviewers for spec, plan and implementation; validate reported findings against source and stop each rolling review once Critical/Medium issues are resolved. Use high-information Doxygen contracts for new C++ seams and update existing Chinese architecture documentation.
