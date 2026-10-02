# Windows V1 Manual Verification Checklist / 手動驗證清單

Use this checklist only for release-candidate manual verification on Windows 11. It defines exactly seven grouped scenarios; it does not add automated test cases. Start every group as **Not run** and change it to **Pass**, **Fail**, or **Skipped** only after recording evidence. A skip must name the unavailable hardware, OS, driver, or supported reproduction method. Store evidence below `artifacts/windows-v1/<run-id>/`; do not add captured pixels, source titles, local config, or other private material to a release archive.

## 1. Source lifecycle

- **Status:** Not run / Pass / Fail / Skipped: `<reason>`
- **Setup:** Provide one capturable display, one capturable window that can be closed, and a second valid source for reselection. Prepare a supported permission-denial condition if the environment offers one.
- **Action:** Refresh the source list; select and start the display, then stop cleanly. Refresh, select and start the window, close the source, and observe the retained overlay. Close the overlay during a separate run. Attempt permission denial where reproducible. After source loss and denial, select the second valid source and start again.
- **Visible expected result:** Refresh replaces the visible snapshot; display and window selections start only the chosen source. Permission denial ends in `Error` without a live overlay. Closing the source shows `Capture interrupted` while retaining the disconnected overlay; closing the overlay follows the ordinary stop path. Reselection starts a new capture after the old runtime has stopped.
- **Diagnostic expectation:** Record the canonical permission error as `PermissionDenied`, source closure as `SourceLost`, overlay closure as `OverlayCloseRequested`, and a safe stop as `Stopped`. There must be no retargeting to a recycled native window and no fabricated `Stopped` while native drain is incomplete.
- **Hardware / OS / driver:** Hardware `<CPU, RAM, GPU(s), display topology>`; OS `<Windows 11 edition, build>`; driver/runtime `<GPU driver, Vulkan runtime, WGC availability>`.
- **Evidence path:** `artifacts/windows-v1/<run-id>/01-source-lifecycle/` containing `environment.txt`, `steps.md`, controller/overlay screenshots or redacted video, and `diagnostics.txt`. Record unavailable denial reproduction as `permission-denial-skip.md`.

## 2. Overlay geometry

- **Status:** Not run / Pass / Fail / Skipped: `<reason>`
- **Setup:** Use a source with a known aspect ratio. Prefer two monitors with different scale factors; otherwise record the single-DPI limitation. Enable both Bordered and Borderless modes.
- **Action:** In each border mode, drag the overlay and resize it from every edge and corner. Cross between mixed-DPI monitors, check centered aspect fit and bars at wide/tall sizes, then minimize to a zero framebuffer and restore repeatedly.
- **Visible expected result:** Bordered mode uses OS chrome. Borderless Editable mode supports caption drag plus DPI-scaled edge/corner resize. Native resize preserves source aspect where applicable and render-time aspect fit remains centered without stretching. `WM_DPICHANGED` adopts the suggested rectangle. Minimize/zero framebuffer pauses presentation without damage, hang, or geometry corruption; restore resumes rendering.
- **Diagnostic expectation:** No terminal runtime error, validation error, tight retry loop, or resource-drain warning is introduced by drag, resize, DPI transition, minimize, or restore. Record any compositor rounding as an observed limitation rather than claiming pixel-exact geometry.
- **Hardware / OS / driver:** Hardware `<GPU(s), monitor models, resolutions, refresh rates, scale factors>`; OS `<Windows 11 edition, build, per-monitor DPI settings>`; driver/runtime `<GPU driver, Vulkan runtime>`.
- **Evidence path:** `artifacts/windows-v1/<run-id>/02-overlay-geometry/` containing `environment.txt`, `geometry-matrix.csv`, screenshots/video for both border modes and DPI transitions, plus `diagnostics.txt`.

## 3. Pass-through recovery

- **Status:** Not run / Pass / Fail / Skipped: `<reason>`
- **Setup:** Place an interactive window owned by another process beneath the overlay. Prepare both overlay border modes, a confirmed global hotkey, and a tool such as Spy++ or a debugger capable of reading the overlay class style.
- **Action:** Record `GetClassLongPtrW(hwnd, GCL_STYLE)` for the actual GLFW overlay class and verify `(style & (CS_OWNDC | CS_CLASSDC)) == 0`. In Bordered and Borderless modes, enter Pass-through and click/drag/type in the foreign process below. Restore Editable once from the controller and once with the confirmed global hotkey.
- **Visible expected result:** The foreign process receives real pointer input beneath the opaque overlay in both border modes. The controller and global hotkey each restore Editable, after which the overlay receives editing input again. Airelinna performs no click forwarding and synthesizes no pointer or keyboard input.
- **Diagnostic expectation:** Preserve the raw class-style value and decoded forbidden-bit check. Mode transitions remain ordinary `AppCommand` handling; no frontend or hotkey callback reaches the overlay/native runtime directly. Record target-process input/message evidence sufficient to distinguish OS hit testing from injected input.
- **Hardware / OS / driver:** Hardware `<GPU, display and input devices>`; OS `<Windows 11 edition, build>`; driver/runtime `<GPU driver, Vulkan runtime, GLFW 3.4 patched-source identity>`.
- **Evidence path:** `artifacts/windows-v1/<run-id>/03-pass-through/` containing `class-style.txt`, `environment.txt`, bordered/borderless video, target-process input trace, and `diagnostics.txt`.

## 4. FPS behavior

- **Status:** Not run / Pass / Fail / Skipped: `<reason>`
- **Setup:** Use a visibly moving source, a display fast enough to observe the requested caps, a timestamp/present measurement method, and a CPU profiler or system trace. Record compositor, VSync, and source-rate limits.
- **Action:** Run 30, 60, 144, and Unlimited settings for the same measured interval. Include a missed-deadline interval and an idle/no-new-frame interval. Record delivered presentation cadence and process CPU use rather than inferring them from the selected radio button.
- **Visible expected result:** 30/60/144 act as upper caps subject to source, compositor, and hardware limits. Unlimited removes the application cap but makes no promise beyond those limits. Rendering uses the newest available frame; no interpolated frames or interpolation claim appear.
- **Diagnostic expectation:** The main loop uses future deadlines/event waits and does not busy-spin when capped, idle, minimized, or waiting for acquire. Explain measurement variance; do not fail solely because observed cadence is below a cap imposed by the environment.
- **Hardware / OS / driver:** Hardware `<CPU, GPU, monitor refresh rate>`; OS `<Windows 11 edition, build, compositor/VSync state>`; driver/runtime `<GPU driver, Vulkan runtime>`.
- **Evidence path:** `artifacts/windows-v1/<run-id>/04-fps/` containing `environment.txt`, `cadence.csv`, profiler/system trace, `method.md`, and `diagnostics.txt`.

## 5. Hotkey conflict

- **Status:** Not run / Pass / Fail / Skipped: `<reason>`
- **Setup:** Start with a known desired chord whose controller binding reads `confirmed`. Reserve a different valid chord in another process so Windows rejects its registration.
- **Action:** Attempt to replace the current chord with the reserved chord, observe the notice and desired chord, enter Pass-through, and invoke the previous chord. Release the external reservation only after evidence is captured.
- **Visible expected result:** Replacement failure reports the conflict, preserves the previous desired chord, and preserves `confirmed` only if the previous OS registration still exists. The previous confirmed chord restores Editable. If an independently observed cleanup or registration loss makes the binding inactive, the controller must instead show `unconfirmed`; it must never claim an inactive binding is confirmed.
- **Diagnostic expectation:** The failed replacement is `HotkeyConflict`; desired configuration is unchanged and no config save is attributed to the rejected chord. Record the native detail when available. Any cleanup failure or registration-loss edge must retain its distinct canonical code and truthful confirmed/inactive projection.
- **Hardware / OS / driver:** Hardware `<keyboard/input method>`; OS `<Windows 11 edition, build, active keyboard layout>`; driver/runtime `<input or remapping software, reserving process and version>`.
- **Evidence path:** `artifacts/windows-v1/<run-id>/05-hotkey-conflict/` containing `environment.txt`, before/after controller screenshots, reserving-process proof, restore video, config diff with private values redacted, and `diagnostics.txt`.

## 6. Screenshot visibility, warnings and graphics recovery

### Controller frame acceptance

- **Status:** Not run. The automated frame and native hit-test checks are recorded separately from these physical interactions.
- **Action:** Launch the rebuilt controller and inspect the full-client ImGui layout. Drag the title, resize all edges/corners, edit a field, operate a popup, and scroll the settings. Minimize with `_` and restore through the taskbar. Move between displays with distinct DPI settings. With a controlled public-content capture active, close through `X`, then repeat with Alt+F4 in a fresh run.
- **Expected result:** One controller title row remains fixed; controls retain client input; resize margins remain usable at each DPI. Both close paths enter normal application drain. Record screenshot visibility using the subchecks below, plus GPU validation output and the observed exit status.
- **Evidence:** Store controller interaction observations alongside this group's environment and diagnostics. Mixed-DPI, physical drag, restore and capture-active close remain pending until this checklist is executed.

### Screenshot and graphics subchecks

- **Status:** Not run / Pass / Fail / Skipped: `<reason per unavailable subcheck>`
- **Setup:** Use a Vulkan validation-enabled verification build and a controlled source containing public test content. Prepare a screenshot tool. Identify environments that can reproducibly expose active HDR, unavailable color detection, a concrete GPU-interop capability failure, replica graphics recovery, and controller device loss. Do not invent fault triggers; mark each unavailable subcheck skipped.
- **Action:** Exercise these bounded subchecks: (a) include the controller and replica in a screenshot, confirm own-process windows are filtered from window-source selection, and inspect full-display feedback with the replica on the captured display and then on another display; (b) refresh/select an HDR source and, separately, a source whose color query is unavailable; (c) start on hardware that naturally selects CPU copy; (d) use a supported reproducible replica surface/device-recovery trigger; (e) repeatedly resize and minimize/restore the controller while collecting validation and CPU traces; (f) only where a vendor/validation-supported reproducible controller-device-loss trigger exists, activate it and observe process shutdown.
- **Visible expected result:** Both product windows appear in the screenshot. A replica on the captured display can produce recursive imagery; selecting a source window or moving the replica to another display removes that feedback from the source. HDR shows `HDR was detected but is unsupported.` while an unavailable query shows `Color detection is unavailable; SDR is assumed.`; SDR shows neither. CPU copy displays its concrete fallback reason. Recoverable replica graphics failure shows `Recovering graphics` and returns to `Capturing` without replacing source identity. Controller minimize/restore remains responsive. Reproducible controller device loss stops controller work and exits non-zero through host-fatal shutdown; no result is claimed when the trigger is unavailable.
- **Diagnostic expectation:** Record `CaptureExclusionStatus`, distinct `ColorSupport::HdrDetectedUnsupported` / `ColorSupport::UnknownAssumeSdr`, and one `BridgeModeChanged { CpuCopy, non-None reason }`. Replica recovery records start/completion facts. Controller acquire remains timeout-zero and controller acquire/submit/present/release retirement is validation-clean and nonblocking through minimize/restore. Confirmed controller device loss uses host shutdown; it is not sent into the replica runtime as a frontend/native shortcut.
- **Hardware / OS / driver:** Hardware `<CPU, controller GPU, replica GPU, displays, HDR state>`; OS `<Windows 11 edition, build, HDR and graphics settings>`; driver/runtime `<GPU driver(s), Vulkan loader/SDK and validation-layer versions, enabled extensions, KHR/EXT maintenance1 path, fault-trigger tool/method>`.
- **Evidence path:** `artifacts/windows-v1/<run-id>/06-warnings-recovery/` containing `environment.txt`, `subcheck-results.md`, warning screenshots, bridge/recovery trace, Vulkan validation log, CPU trace, and `controller-device-loss.md` with either supported evidence or an explicit skip reason.

## 7. Package and privacy

- **Status:** Not run / Pass / Fail / Skipped: `<reason>`
- **Setup:** Use the final release ZIP, its build-environment record, dependency/notices files, and a clean Windows 11 user profile. Prepare archive, PE/runtime-dependency, filesystem, log, and network inspection tools without placing private capture data in shared evidence.
- **Action:** Verify product name/version and archive SHA-256; enumerate archive contents and runtime dependencies; inspect pinned dependency versions/licenses and the marked GLFW 3.4 no-`CS_OWNDC` alteration. Run from the clean profile, explicitly select/start capture, change hotkey/FPS/border, inspect config keys/logs, and observe outbound network activity. Read the supported-hardware release note.
- **Visible expected result:** Product spelling is `Airelinna` and version matches release metadata. Capture starts only after explicit selection and Start. Config contains only `version`, `hotkey`, `fps_cap`, and `border`; it contains no mode, source, title, pixels, window position, or captured content. Logs contain no source title or pixel content by default, and no pixels/titles are transmitted. The archive contains only intended release artifacts and documented runtime DLLs: no source, test executable, validation layer, local config, captured content, or external `.spv`.
- **Diagnostic expectation:** Runtime dependencies are limited to documented Windows/Vulkan/driver and compiler runtime components; GLFW and Dear ImGui are static and their exact versions/licenses appear in `THIRD_PARTY_NOTICES.md`. `SECURITY.md` documents explicit capture, config/log contents, permissions, no pixel/title transmission, default screenshot visibility, and current-process source filtering. The release note states that both controller and replica presentation devices require present-fence support from `VK_KHR_swapchain_maintenance1` or `VK_EXT_swapchain_maintenance1`; CPU-copy fallback changes only the D3D/Vulkan bridge and does not waive this hardware floor.
- **Hardware / OS / driver:** Hardware `<CPU, GPU(s), display topology>`; OS `<clean Windows 11 edition, build>`; driver/runtime `<GPU driver, Vulkan runtime, compiler runtime DLL versions>`; build `<compiler, CMake preset/configuration, source revision>`.
- **Evidence path:** `artifacts/windows-v1/<run-id>/07-package-privacy/` containing `environment.txt`, `archive-list.txt`, `runtime-dependencies.txt`, `dependency-audit.md`, `config-keys.txt`, redacted `logs.txt`, `network-observation.md`, `release-note-audit.md`, and `sha256.txt`.
