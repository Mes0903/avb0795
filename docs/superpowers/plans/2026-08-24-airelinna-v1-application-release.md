# Airelinna V1 Application and Release Implementation Plan

> **For agentic workers:** Run after P1–P3. This plan completes the frontend/application composition and release evidence. It does not authorize a commit or push.

**Goal:** Build a thin Dear ImGui controller, a single application orchestrator, an event-driven Windows composition root, and auditable packaging without letting the frontend call through to windows, capture, or rendering.

**Architecture:** Controller code sees immutable view data and emits commands into a queue. `Application` alone drains commands, mutates domain state, invokes ports/runtime, and processes runtime events. Concrete Windows construction remains in `main`.

**Tech Stack:** C++23, Dear ImGui with GLFW/Vulkan backends, P1 application ports, P3 Windows runtime, CPack.

---

## 1. Files and responsibilities

| Path | Responsibility |
|---|---|
| `app/app_command_queue.*` | Ordered main-thread command transport, no business logic |
| `app/application.*` | Sole use-case orchestrator and domain/runtime owner |
| `app/controller_view_state.*` | Pure projection from domain/capabilities/config to UI values |
| `include/airelinna/ui/controller_view.hpp`, `src/ui/controller_view.cpp` | Dear ImGui widgets and command emission only |
| `include/airelinna/desktop/desktop_host.hpp`, `src/desktop/desktop_host.cpp` | Controller GLFW/Vulkan/ImGui lifetime and event loop |
| `platform/windows/windows_atomic_replace.*` | One callable used by `ConfigStore` |
| `main.cpp` | Process/COM/GLFW setup and concrete dependency construction |
| `cmake/Packaging.cmake`, Windows resource/manifest file | Install/package metadata and process DPI declaration |
| `cmake/VerifyTestInventory.ps1` | Final CTest-discovered IDs/labels gate; no test execution or source-text counting |
| `docs/verification/windows-v1-checklist.md` | Seven grouped manual scenarios |
| `SECURITY.md`, `THIRD_PARTY_NOTICES.md` | Privacy and dependency disclosure |
| `tests/app/application_contract_test.cpp` | Named case 1 |
| `tests/app/controller_contract_test.cpp` | Named case 7 |

Do not create a presenter hierarchy, navigation framework, dependency container, plugin registry, telemetry service, or a controller-to-runtime adapter.

## 2. Task 1: Add Dear ImGui and application targets

- [ ] Pin Dear ImGui source. `airelinna_ui` compiles widget code only; `airelinna_desktop` privately compiles the GLFW/Vulkan backend sources required by the controller host. The controller sets `GLFW_CLIENT_API` to `GLFW_NO_API` and uses the exact patched GLFW main class established by P3; no OpenGL context path is enabled.
- [ ] Add `application.cpp`, `app_command_queue.cpp`, and `controller_view_state.cpp` to the P1-created `airelinna_app` target; do not recreate or rename that target.
- [ ] Create `airelinna_ui` after `controller_view.cpp` exists; link only `airelinna_core` and Dear ImGui. Its two permitted application includes are `app/commands.hpp` and `app/controller_view_state.hpp`; it does not link `airelinna_app`, GLFW, Vulkan, `airelinna_windows`, or `airelinna_windows_graphics`.
- [ ] Create `airelinna_desktop` after `desktop_host.cpp` exists; it links `airelinna_app`, `airelinna_ui`, GLFW, Vulkan, and the ImGui platform/render backends, but no replica-runtime target. It may include the header-only acquire/submit/present-result classifiers from P3; those pure helpers expose only `VkResult` categories and no replica owner or operation.
- [ ] Add application case 1 and controller case 7 to the existing deterministic test executable.
- [ ] Create the product executable only after `main.cpp` and all linked targets exist.
- [ ] Use a console executable while the entry point is `main`. A later GUI subsystem switch must add a correct `wWinMain` and retained diagnostic path in the same change.

Run target-graph generation after each target is introduced. Every include root is `${PROJECT_SOURCE_DIR}/include`. After all P4 targets exist, call P1's `airelinna_assert_target_boundaries()` so exact source-root and direct-link allowlists fail during configure; keep the text scan as the one `verify_source_boundaries` build target.

## 3. Task 2: Define the one-way frontend contract

### Controller view state

`ControllerViewState` is a portable value containing:

- the current portable `SessionId` used to construct every session-specific command;
- source rows with source ID, kind-derived display/window glyph, text label, and selected flag;
- lifecycle/status/error text;
- current mode, FPS cap, border preference, desired typed hotkey display text, and whether that desired chord is currently a confirmed active OS binding;
- current effective bridge mode/reason when a runtime has reported one;
- one application-level notice for hotkey/config/command failures that are not session state;
- enabled/disabled state plus explanation for controls affected by capabilities or lifecycle;
- one selected-source color warning derived only from portable `ColorSupport`: none for `Sdr`, HDR-unsupported for `HdrDetectedUnsupported`, or color-detection-unavailable/SDR-assumed for `UnknownAssumeSdr`; plus independent capture-exclusion warnings.

It contains no pointer, callback, native handle, texture, runtime reference, renderer reference, or platform object.

`build_controller_view` is one pure projection from `ApplicationState`, current `SessionId`, source snapshot, static capabilities, config, the Application-owned `hotkey_binding_confirmed` bit, controller exclusion, the complete runtime-feedback value, runtime-present/`stop_requested` facts, and the optional application notice. These are portable values supplied by `Application`; the projection never receives the runtime, a port, a service, or a native object. Keep it as a function or one concrete value builder; do not create an interface or a second shadow state for it.

### Controller view

`ControllerView::render` receives `const ControllerViewState&` and one `CommandSink` after `DesktopHost` begins an ImGui frame. User gestures emit `AppCommand` values only:

- refresh/select/start/stop;
- set Editable or Pass-through;
- set FPS or border;
- replace hotkey;
- quit.

The controller never calls `IReplicaRuntime`, `IGlobalHotkey`, `ConfigStore`, `OverlayWindow`, GLFW-native access, or Windows/render classes. It does not maintain a shadow session state; widget-local text editing is allowed until a command is submitted.

### Test case 7

One table/section-driven case verifies the pure `build_controller_view` projection:

- display/window text rows and selected state;
- current `SessionId` projection for session-specific commands;
- no live-thumbnail/pixel field;
- application notice plus current bridge mode/reason—including `InteropOperationFailure` without another event type—and Normal/Stalled drain projection;
- all three `ColorSupport` projections, including distinct `HdrDetectedUnsupported` and `UnknownAssumeSdr` text, plus independent controller/overlay exclusion unsupported/failed warnings;
- capability-disabled controls and explanations;
- lifecycle/capability combinations produce the intended enabled states and explanations, including Refresh disabled for every retained-runtime state such as Interrupted;
- Error with no runtime permits a new Start, while the same visible Error with a retained draining runtime disables Start until `Stopped`.

Command emission remains a thin widget-to-value mapping exercised by the seven grouped manual scenarios and the source-boundary gate. Do not add an ImGui test engine, one test per widget, command-builder helpers solely for tests, or draw-list snapshots.

## 4. Task 3: Implement ordered main-thread command transport

User and hotkey commands must not be newest-only because Start followed by Stop cannot be collapsed. Both producers run on the main thread, so `AppCommandQueue` is a concrete `std::deque<AppCommand>` with `push`, `drain`, and `close`; it has no mutex, background worker, priority protocol, or message-bus interface.

The queue is drained before each UI render. Runtime callbacks cannot access it; callback state remains inside the current runtime and is returned by its next main-thread `tick`. This transport has no separate top-level test; case 1 and the grouped controller scenarios exercise its ordering.

## 5. Task 4: Implement `Application` as the sole orchestrator

`Application` owns:

- `ApplicationState` and the current session ID;
- current source snapshot and `PlatformCapabilities`;
- loaded `UserConfig` plus one `config_dirty` bit owned only by `Application`;
- one `hotkey_binding_confirmed` bit, initially false; when true, the confirmed OS binding is exactly the current in-memory desired chord in `UserConfig`;
- one host-facing `ExitDisposition`, initially `Continue` and owned only by `Application`;
- one optional portable application notice, cleared by a successful retry of the same operation;
- controller `CaptureExclusionStatus` supplied at construction;
- one runtime-scoped feedback value containing current-session overlay exclusion (initially `NotAttempted`), optional bridge mode/reason, and drain status (initially `Normal`);
- one optional pending replacement Start value, quit intent, and one private `stop_requested` bit independent of visible session lifecycle;
- one optional application-owned immediate-step deadline;
- at most one `std::unique_ptr<IReplicaRuntime>`;
- references to `ISourceCatalog`, `IReplicaRuntimeFactory`, `IGlobalHotkey`, `ConfigStore`, and the command queue.

It does not own D3D/Vulkan/WGC/GLFW-native resources and does not receive a captured frame.

During initialization, load and validate config, create the first idle session, refresh sources, and attempt to register the stored typed hotkey. Only success sets `hotkey_binding_confirmed`; `AppErrorCode::HotkeyConflict`, `AppErrorCode::HotkeyCleanupFailure`, or `AppErrorCode::HotkeyRegistrationLost` leaves it false, is stored in the application notice, and does not prevent startup. The independent controller remains the recovery route, and the stored desired chord is not silently replaced.

### Command handling

All commands are drained and processed on the main thread:

The design spec's transition table is normative. In particular, Error with no runtime can enter Starting, Interrupted with a retained runtime can enter Stopping for one pending replacement Start, and Error with a retained draining runtime cannot Start or Refresh until safe `Stopped` releases that runtime.

- Refresh is accepted only in Idle or Error **with no retained runtime**. It calls only `ISourceCatalog`, retires/replaces the source snapshot, and clears a selected source absent from the new generation. Interrupted and every other retained-runtime state disable Refresh, so a registry generation cannot revoke an observation leased by a live runtime; the user must Stop and receive safe `Stopped` first.
- Select mutates domain selection only when lifecycle allows it.
- Start from Idle or Error with no old runtime transitions to Starting, calls the runtime factory with source/settings only, and publishes the runtime plus transitions to Capturing only after full construction succeeds; failure transitions to Error with no runtime. Publication resets `stop_requested`. If an Interrupted runtime exists, retain exactly one pending Start value, transition to Stopping, set `stop_requested`, and call `request_stop()`; construct the replacement only after that runtime later reports `Stopped`. A further Start while stopping is rejected rather than queued/replaced. Factory failure leaves no runtime or callback path that can later reach `Application`.
- Stop from Capturing, Interrupted, or RecoveringGraphics transitions to Stopping, cancels any pending replacement Start, sets `stop_requested`, calls idempotent `runtime.request_stop()`, and retains the runtime. Only its later `Stopped` event permits release and transition to Idle.
- Mode first forms candidate session settings. When a runtime is active, call `runtime.apply` and update domain state only after it succeeds. Mode is session-only in V1: it never changes `UserConfig` and never triggers a config save.
- FPS/border first form candidate settings. When a runtime is active, call `runtime.apply` and update domain state plus the corresponding `UserConfig` field only after it succeeds; without a runtime, update those two values directly. For either accepted config-backed change, set `config_dirty` before calling `ConfigStore::save` and clear it only after success. A save failure becomes an application notice without rolling back the already-applied in-memory/runtime setting or clearing dirty state.
- For either settings path, an ordinary native-policy failure means the runtime has restored and verified its previous native settings. Terminal `RuntimeStateLost` moves the session to Error, sets `stop_requested`, requests safe stop, and retains the runtime until `Stopped`; visible Error is deliberately not replaced by Stopping.
- Hotkey replacement calls `IGlobalHotkey::replace`; success updates the desired chord and sets `hotkey_binding_confirmed`, then sets `config_dirty` and uses the same save/clear-on-success rule as FPS/border. `AppErrorCode::ConfigWriteFailure` does not roll back the active binding or accepted in-memory chord. `AppErrorCode::HotkeyConflict` and `AppErrorCode::HotkeyCleanupFailure` preserve the current confirmation bit and desired chord: this truthfully covers either a still-confirmed old primary or an inactive/unconfirmed primary left by an earlier loss. `AppErrorCode::HotkeyRegistrationLost` clears the bit while retaining the prior desired chord and keeping the controller available. Any registration error replaces the application notice; a successful retry clears the related notice.
- `RestoreActiveReplicaEditable` is resolved only by `Application`: while `stop_requested` is false, apply Editable to the current Capturing/Interrupted/Recovering runtime-bearing session; otherwise perform a harmless no-op. The hotkey adapter never receives a `SessionId` or runtime reference.
- Quit and public `shutdown()` enter the same private idempotent `begin_shutdown(now)` path: cancel pending replacement, set quit intent, stop accepting normal commands, clear the hotkey and clear `hotkey_binding_confirmed`, retain an unsaved-settings diagnostic when `config_dirty` is already set, and do not retry synchronous config I/O. When a runtime exists, set `stop_requested` before requesting stop. Immediately after that no-throw stop initiation, inspect the already-retained runtime drain status: if it is already `Stalled`, set `ExitDisposition::AbandonProcess` without waiting for another status edge; otherwise the host keeps stepping until clean `Stopped` or a later stalled edge. With no runtime, finalization sets `ExitDisposition::Clean`. No frontend or composition-root code performs those operations separately.

Entering Pass-through does not require frontend access to the overlay. The command reaches `Application`, which updates settings and calls `runtime.apply`. Controller and registered hotkey both remain independent recovery paths.

### Runtime events

`IReplicaRuntime::tick(now) noexcept` synchronously returns the fixed eight-slot `RuntimeTickResult` for the currently owned runtime. Its events were formed from runtime-owned source/job callback state on the main thread; no callback holds an application sink/reference, and no application runtime-event queue or epoch exists. Destroying/replacing the safely stopped runtime structurally removes every future event path from that instance.

On runtime publication, and again when that runtime is released, `Application` resets all runtime-scoped feedback so a replacement cannot inherit the prior overlay-exclusion warning, bridge reason, or drain warning. Recovery lifecycle/generation belongs to `ReplicaSession` and is cleared by its publication/release transition rather than duplicated in the feedback value. The controller-window exclusion result is process-scoped and is not reset.

For each result, `Application` first partitions status-only events from lifecycle-driving events:

- `BridgeModeChanged` replaces the current bridge mode/reason; no second degraded event exists;
- overlay capture-exclusion updates only the current runtime warning;
- `RuntimeDrainStatusChanged { Stalled }` exposes a warning while retaining the runtime and all native leases, and `{ Normal }` clears it after progress resumes. It covers callback/job cleanup as well as graphics retirement. Stalled grants no release permission during normal operation. If quit/host-fatal intent is already active and hotkey cleanup, dirty-config diagnostic retention, and stop initiation have completed, a new Stalled edge sets `ExitDisposition::AbandonProcess`; `begin_shutdown` separately checks the retained status so a pre-existing Stalled state needs no duplicate edge. Neither path releases the runtime or fabricates `Stopped`;
- overlay close requested from any mode/border combination enters the same Stop transition as the controller command; the runtime/window never destroys itself;
- source lost updates Interrupted while retaining the runtime/overlay;
- recovery started/recovered update lifecycle and generation status;
- `Failed { AppErrorCode::RuntimeStateLost }` carries only the allocation-free fixed `AppError` value, clears pending Start, updates Error, sets `stop_requested`, requests stop, and retains the runtime until a later `Stopped` or stalled process-abandon disposition; no platform-local terminal error code is accepted;
- stopped is the sole normal release point. Release the empty runtime and clear `stop_requested`; then quit sets `ExitDisposition::Clean`, otherwise a pending replacement starts, otherwise Error remains visible, and only a normal Stop transitions to Idle.

Status-only events are applied before one winning lifecycle outcome. The runtime does not rank portable candidates. When `stop_requested` is false, `Application` alone selects that outcome independent of fixed-slot order using `Failed > OverlayCloseRequested > SourceLost > recovery`; once selected, it ignores every lower-priority outcome. If the selected recovery outcome contains both `GraphicsRecoveryStarted` and `GraphicsRecovered`, Application applies the two causal transitions in that order during the same step, so an immediately completed recovery cannot strand visible state in Recovering. A lone `GraphicsRecovered` remains valid only from an already-Recovering session. In `Interrupted`, either recovery fact is instead a harmless self-transition: the runtime may repair retained graphics internally, while the visible source-disconnected lifecycle remains dominant; a terminal `Failed` still moves it to Error under the higher-priority rule. When `stop_requested` is true, Application ignores all lifecycle events except `Stopped`, regardless of whether the visible lifecycle is Stopping or Error. The runtime's stop-phase contract requires `Stopped` alone after safe teardown; repeated exceptional stop ticks instead retain a bounded deadline and contribute sticky drain uncertainty/Stalled. An unexpected `Stopped` without `stop_requested` is treated as a runtime protocol failure: release the reported-empty runtime and preserve Error rather than leaking it. `FatalCaptureError > SourceLost > Resize` is already coalesced below the runtime port.

If event processing calls `request_stop()`, publishes a replacement runtime after `Stopped`, or otherwise creates work after the one runtime tick for this step has already occurred, `Application` sets its private immediate-step deadline to `now`. `shutdown()`, which may be called between steps, sets the same deadline from `steady_clock::now()`. This deadline is combined with the runtime deadline by `next_deadline()`; it does not add a public callback or a second tick inside one step.

### Test case 1

Use one small recording implementation of each real application port; do not create a general mock framework. Case 1 has six flat named `SECTION` groups, using scripted rows rather than nested combinations:

1. startup/source ownership: the normative Idle/Error-without-runtime to Starting rows, factory success/failure, selectable errors, stale-`SessionId` command rejection, Refresh rejection while Interrupted retains a runtime, Interrupted to Stopping for replacement, and replacement construction only after old `Stopped`;
2. runtime feedback: Capturing/Recovering source loss to Interrupted, mode/border-independent overlay close to Stopping, recovery start/completion, recovery self-transitions that preserve Interrupted, Error-with-retained-runtime release behavior, status replacement/reset, multiple lifecycle-candidate orders with Application-only arbitration, a same-batch recovery start/completion pair, stop-phase suppression, and unexpected `Stopped`;
3. settings: ordinary versus terminal `apply`, session-only Mode, and exact FPS/border config-dirty/save behavior;
4. hotkey/notices: session-agnostic restore in active/idle/stopping states; desired-versus-confirmed projection; successful replacement; conflict and probe-cleanup preserving the current confirmed state; `RegistrationLost` clearing it; a following primary `CleanupFailure` keeping it false; config notice preservation; and successful retry;
5. stop/shutdown: idempotent stop, Error preservation, pre-existing/new Stalled abandonment, hotkey cleanup plus dirty-config no-retry before drain, and release only after safe `Stopped`;
6. scheduling: runtime no-deadline plus overlay-close/failure/shutdown/replacement work produces the Application-owned immediate next step.

The case must not inspect Windows objects or reach into a concrete runtime.

### Application step

`Application::step(now) noexcept` performs application work only:

1. clear the consumed immediate-step request and drain commands;
2. call the current runtime `tick(now)` once, process status-only feedback and the one arbitrated lifecycle outcome (including both causal transitions of a selected recovery pair), and retain its optional next wake deadline;
3. retain any new application immediate-step deadline created after that tick;
4. build and retain one `ControllerViewState` for the host.

`DesktopHost::run(Application&)` owns the loop around that step: begin controller ImGui frame, render `ControllerView`, acquire/present through its private controller swapchain, and wait with `glfwWaitEventsTimeout` using the earliest runtime/application/UI/graphics deadline. Controller acquire always calls `vkAcquireNextImageKHR` with timeout zero and applies the shared `classify_acquire_result`: success/suboptimal alone owns the image and both acquire signal operations and may submit; every unsuccessful result owns no image, ignores the output index, and leaves the supplied semaphore/fence unaffected; `VK_NOT_READY`/`VK_TIMEOUT` returns a strictly future bounded retry deadline and does no submit/present; out-of-date/full-screen loss requests recreation; surface/device loss and terminal errors enter host-fatal shutdown. Every controller present chains its own KHR/EXT present fence and applies `classify_present_result`; results known to enqueue WSI work retain their fence, wait semaphore, and retired swapchain until signaled. A known not-enqueued OOM releases only the unused present fence; its acquired image and render-finished semaphore stay with the submission record until its fence signals, then the host calls maintenance1 and applies `classify_release_result`. Only `Released` clears the image; `SurfaceLost` diverts to controller surface teardown and `Uncertain` becomes terminal, with failed outcomes retained under the swapchain until legal destruction. The completed semaphore is destroyed/replaced rather than reused. Unknown present results fail closed. The controller command sink also sets one host-local `command_emitted` flag; a set flag immediately continues to the next application step instead of sleeping with a queued command. No deadline means the host waits for an OS, UI, or callback wake rather than polling; an incomplete runtime drain always supplies its own bounded observation deadline. There is no unlimited `glfwPollEvents` busy loop. Controller close and Quit both enqueue an explicit Quit command and set the same flag.

Each controller acquire supplies both a binary semaphore and an acquire fence; neither is host-waited. The host applies `classify_submit_result` to every submit. Success owns the normal record. On submit OOM, which is guaranteed not enqueued, it polls the acquire fence, then calls maintenance1 for the unused image and classifies the result. Only `Released` clears the image; `SurfaceLost` diverts to surface teardown and `Uncertain` becomes terminal, with failed outcomes retained under the swapchain until legal destruction. The completed acquire semaphore is destroyed/replaced while unaffected command, render-finished semaphore, and unused submission fence state may be recycled or destroyed. Device loss uses the existing lost-device path. Unknown/other submit failure retains the complete uncertain record; a later signaled submission fence permits retirement, but otherwise controller drain reaches Stalled and returns `abandon_process` without unwinding. This is private controller state and exposes no runtime operation.

The host checks every controller acquire/submit/present/release result. On controller device/surface loss or another terminal controller-graphics failure, it stops controller rendering, calls only public `Application::shutdown()`, continues headless `Application::step` calls at returned deadlines until `exit_disposition() != Continue`, and records the fatal diagnostic. Surface loss still uses ordinary submission/presentation retirement and attempts maintenance release of any unpresented acquired image only after all uses complete; a failed release retains the record under the old swapchain until legal swapchain destruction rather than falsely clearing it. Only confirmed Vulkan device loss uses lost-device child cleanup without waiting failed Vulkan fences. Other terminal results fail closed and retain possibly referenced resources unless their classified proof permits release. If the controller's own fail-closed drain reaches Stalled, the host returns a concrete `{ nonzero_exit_code, abandon_process = true }` result after Application finalization; otherwise it returns nonzero with normal controller cleanup. It never reaches a replica port/runtime.

The whole `Application::step` body has one outer exception firewall. Known port/native failures remain typed results; an unexpected exception sets a fixed emergency diagnostic, calls only no-throw `request_stop()`/`clear()` operations, immediately selects `ExitDisposition::AbandonProcess`, and returns while every possibly live runtime owner remains retained. It skips allocation-prone notice/config work in that exceptional path. `DesktopHost::run` has the corresponding outer controller/UI firewall: stop new controller operations, invoke only `Application::shutdown() noexcept`/headless `step() noexcept` until an application disposition exists, then return `abandon_process = true` regardless of whether Application drained cleanly, so `main` never unwinds uncertain controller state. Neither firewall is an interface, service, or ordinary recovery path.

`DesktopHost` is constructed before `Application`. Its existing window-created callable, supplied by `main`, first applies the same small Windows class-style validator to the controller `HWND`, then attempts capture exclusion; it returns `std::expected<CaptureExclusionStatus, AppError>` without exposing the native handle to `ui` or `Application`. A forbidden `CS_OWNDC`/`CS_CLASSDC` bit proves the pinned GLFW patch is absent and returns `AppErrorCode::UnsupportedWindowClass` before `Application` or an overlay can be created. On success the host stores only the portable exclusion status and exposes it to the composition root through a read-only accessor. `main` passes that value plus one portable static `PlatformCapabilities` value for the Windows 11 baseline into the `Application` constructor. The value has no GPU bridge field; bridge status can come only from a runtime event. No capability port or post-construction host-to-application reporting method is added.

`desktop_host.hpp` also defines one concrete `HostRunResult { int exit_code; bool abandon_process; }` used only by `main`; it is not a port, service, session state, or UI value. `abandon_process` is true only for Application `AbandonProcess` or a controller-owned stalled fatal drain.

During its private Vulkan-device construction, `DesktopHost` enforces the documented V1 supported-hardware floor by requiring and enabling the present-fence feature from `VK_KHR_swapchain_maintenance1` or `VK_EXT_swapchain_maintenance1`; absence returns `AppErrorCode::UnsupportedGraphics` as a process-startup failure, even if CPU-copy capture would otherwise work. It owns an optional active swapchain plus a retired set. Acquire is timeout-zero only; pending results reuse synchronization objects only after confirming they remain unsignaled/unassociated and schedule a positive deadline rather than a same-iteration loop. Recreate stops new acquire/submission and, after image use completes, attempts classified maintenance release for every acquired image that will not be presented. Same-surface creation proceeds only when all required releases are `Released`; `SurfaceLost` diverts to surface teardown and `Uncertain` is terminal, with failed outcomes retained under the old swapchain until legal destruction. Immediately before a permitted creation call, the host moves the current handle to the retired set and passes it as `oldSwapchain`; because that call retires the old handle even on failure, failure leaves no active swapchain and a bounded retry uses `VK_NULL_HANDLE`, never the retired handle again. On normal exit and surface loss it stops controller submissions and continues deadline-aware present-fence polling before destroying controller presentation resources. It never treats an acquire/submission fence or `vkDeviceWaitIdle` as presentation retirement. Confirmed controller Vulkan device loss alone is the explicit host-fatal abandonment path. This state belongs only to the controller Vulkan owner and exposes no replica/runtime operation.

After its composition-root constructor, `Application` exposes only `step(now) noexcept`, `view_state() noexcept`, `next_deadline() noexcept`, `exit_disposition() noexcept`, and `shutdown() noexcept` as public operational methods. `next_deadline()` returns the earlier of runtime and private immediate-step deadlines. `shutdown()` invokes the private idempotent shutdown orchestration above, performs no config file I/O, and does not synchronously drain native resources. Source/runtime/hotkey/config orchestration helpers are private. `DesktopHost` includes only `app/application.hpp`, `app/commands.hpp`, and `app/controller_view_state.hpp`; it cannot include `app/ports.hpp` or obtain a service/runtime.

## 6. Task 5: Guarantee shutdown and error visibility

Normal application shutdown:

1. stop accepting normal commands; Quit remains idempotent, and Editable restore is a harmless no-op once `stop_requested` is set;
2. clear/unregister global hotkey;
3. when `config_dirty` is already set from an earlier command-time save failure, retain an unsaved-settings diagnostic but make no shutdown-time `ConfigStore::save` call;
4. call `runtime.request_stop()` and continue deadline-driven `step` calls while retaining it;
5. release the runtime only after `Stopped`, then set `ExitDisposition::Clean`;
6. if the retained quit/fatal drain instead reports Stalled, keep every native owner alive, set `ExitDisposition::AbandonProcess`, and never emit/accept a fabricated `Stopped`.

Runtime stop provides the deeper WGC/mailbox/GPU/window order from P3. `Application` does not reproduce or reach into those steps. For `Clean`, `DesktopHost` stops new controller presents and retires its own pending present fences/resources before `run` returns. For `AbandonProcess`, or a controller-owned drain that itself becomes Stalled, the host returns `{ abandon_process = true }` without destroying native owners; `main` flushes diagnostics and calls `std::_Exit(nonzero)` before stack unwinding. This controller/process path is not an Application command or replica-runtime call.

Constructor or startup failures use the P1 `AppErrorCode` inventory and return typed errors to the controller where it can remain available. Process-level failure before controller creation is written to the diagnostic stream with its exact code and returned as a documented non-zero exit code; no composition-root-only error enum is added.

## 7. Task 6: Build the Windows composition root

`main.cpp` performs construction only. Before creating any window, establish `PER_MONITOR_AWARE_V2` through the packaged manifest or one early Windows helper and verify the effective process mode; an already-established equivalent mode is success, while a different effective mode returns `AppErrorCode::RuntimeConstructionFailure`. Then initialize C++/WinRT as an MTA on the same main thread that owns GLFW, and uninitialize it only after every WGC/Windows object is destroyed:

```text
establish/verify PER_MONITOR_AWARE_V2 before window creation
-> initialize COM/WinRT for the main thread
-> initialize GLFW
-> create stable AppCommandQueue and wake callable
-> create GLFW_NO_API DesktopHost/controller with required KHR/EXT present-fence support;
   window-created callable verifies the patched class style and returns controller CaptureExclusionStatus
-> create WindowsSourceRegistry
-> create WindowsSourceCatalog and WindowsRuntimeFactory borrowing that registry; factory also receives the stable main-loop wake callable
-> create WindowsGlobalHotkey with command sink
-> create ConfigStore with Windows atomic-replace callable
-> create portable static Windows 11 PlatformCapabilities (no GPU bridge claim)
-> create Application from those dependencies plus static capabilities and controller exclusion status
-> DesktopHost runs Application steps and ControllerView
-> DesktopHost requests Application shutdown and continues headless/normal steps until exit_disposition != Continue
-> for Clean, DesktopHost retires its own pending controller presents without blocking WaitIdle and returns a normal HostRunResult
-> for Application/controller stalled drain, DesktopHost returns HostRunResult { nonzero, abandon_process = true } with all unsafe owners retained
-> main flushes diagnostics and calls std::_Exit immediately for abandon_process; no destructor or teardown step below runs
-> otherwise destroy Application; retire registry item observations and destroy Windows services; then destroy DesktopHost
-> terminate GLFW and COM
```

The command queue is declared before and destroyed after every producer/consumer. `DesktopHost` is declared before services/`Application`, so it remains available for final error display but is destroyed after them. Concrete services do not capture an `Application&`; the hotkey captures only its stable command sink, frame callbacks retain state inside their owning job, and item-`Closed` callbacks retain only registry observation state plus a weak runtime wake target. `DesktopHost` has no reference to a port, service, `IReplicaRuntime`, or concrete Windows runtime; it sees only the public host-facing `Application` surface listed above.

`WindowsGlobalHotkey` receives a real hidden message `HWND`, never null. Its `noexcept` WndProc contains command-sink exceptions locally and never calls `Application`; the message pump remains integrated with the GLFW main-thread loop. `main` has no session logic, source selection logic, capture callback, renderer recovery logic, or test-only scenario switch; its only exceptional branch is consuming `HostRunResult::abandon_process` and invoking non-unwinding process exit.

### Windows atomic replace

Provide one concrete callable:

- if the destination exists, use an atomic supported replacement path such as `ReplaceFileW`;
- if it does not exist, use a same-directory move with replace/write-through semantics;
- translate Win32 errors to `AppErrorCode::ConfigWriteFailure`;
- do not export a virtual filesystem or OS file interface.

## 8. Task 7: Manual verification in seven grouped scenarios

Create one checklist with setup, action, visible expected result, diagnostic expectation, hardware/OS/driver fields, and evidence path for each group:

1. **Source lifecycle:** refresh, display/window selection, permission denial, close/loss, interrupted overlay, reselection.
2. **Overlay geometry:** bordered/borderless, drag, edge/corner resize, aspect bars, mixed DPI, minimized/zero framebuffer.
3. **Pass-through recovery:** record the actual GLFW class style with neither forbidden DC bit; a window owned by another process receives mouse input beneath bordered/borderless overlay; controller and global hotkey restore Editable; no click forwarding or synthesized input.
4. **FPS behavior:** 30/60/144/Unlimited caps, no interpolation claim, no busy spin.
5. **Hotkey conflict:** replacement failure preserves the previous desired chord and the truthful current confirmed/inactive state.
6. **Warnings/recovery:** capture exclusion warning, distinct `HdrDetectedUnsupported` and `UnknownAssumeSdr` selected-source warnings, CPU fallback reason, replica graphics recovery, validation-clean nonblocking controller acquire/present retirement including minimize/restore, and controller-device-loss host-fatal shutdown only when the environment exposes a supported reproducible trigger.
7. **Package/privacy:** config keys, logs, no pixel/title transmission, dependencies, version/name, archive contents, and a clear release note that KHR/EXT swapchain-maintenance present fences are the V1 supported-hardware floor rather than a CPU/GPU bridge capability.

These are seven manual scenarios, not seven new automated executables. Hardware skips and limitations are recorded honestly.

## 9. Task 8: Package an auditable release

- [ ] Implement `VerifyTestInventory.ps1` as one Windows PowerShell 5.1/PowerShell 7-compatible final gate over the complete `ctest --show-only=json-v1` test array after dependency tests are disabled and all Airelinna tests are built. Require exactly 15 total discovered tests, require every test to carry `airelinna-v1`, require names/unique IDs 1-15, and require `hardware` on exactly IDs 13-15. Inspect the JSON command/name/properties fields; do not prefilter by label, run tests, scan source text, or add a `pwsh` dependency.
- [ ] Add Airelinna version/icon resources and correct product spelling.
- [ ] Build GLFW and Dear ImGui statically; do not package their DLLs.
- [ ] Confirm the replica pipeline creates shader modules from P3's linked embedded byte arrays and the statically linked Dear ImGui Vulkan backend introduces no shader-file dependency; the archive contains no external `.spv` and startup performs no shader-file I/O.
- [ ] Include `THIRD_PARTY_NOTICES.md` with pinned dependency versions/licenses.
- [ ] Include `SECURITY.md` stating explicit capture start, no transmitted pixels/titles, config contents, logging defaults, permissions, and best-effort exclusion.
- [ ] Use CPack ZIP with the executable and notice/security files.
- [ ] Inspect runtime dependencies; Windows/Vulkan/driver and documented compiler runtime DLLs are allowed.
- [ ] Exclude validation layers, source files, test executables, captured content, and local config.
- [ ] Record archive SHA-256 and named build environment.

Do not add updater, installer framework, crash telemetry, analytics, signing service, or plugin packaging in V1.

## 10. Application/release completion gate

- [ ] Cases 1 and 7 pass; the complete CTest JSON inventory contains exactly 15 uniquely numbered `airelinna-v1` tests, with `hardware` on exactly cases 13-15, unless the user explicitly approves a new top-level case.
- [ ] `ui` links no GLFW/Vulkan/Windows/graphics target and contains no native/runtime call.
- [ ] `ui` also links no `airelinna_app` and includes only the two explicit command/view-state contract headers from that layer.
- [ ] `desktop` owns controller rendering and the event loop but has no replica-runtime access.
- [ ] The configure-time target assertion enforces exact source-root and direct-link allowlists, and the single text-scan target covers core, app, UI, desktop, Windows adapters, render code, and `main.cpp`.
- [ ] All frontend actions become `AppCommand`; all runtime feedback becomes `RuntimeEvent` before domain mutation.
- [ ] Application owns exactly one runtime in V1 and never handles native frames.
- [ ] Main loop is deadline-aware, schedules an application-owned immediate step for post-tick work, honors every incomplete-runtime-drain observation deadline, and branches only on the Application-owned `ExitDisposition`: `Clean` unwinds normally, while `AbandonProcess` flushes diagnostics and calls non-unwinding `std::_Exit(nonzero)` with native owners retained.
- [ ] `Application` alone owns config dirty state; accepted hotkey/FPS/border changes set it before save, command-time success clears it together with the related notice, failures retain it, and shutdown preserves the unsaved-settings diagnostic without retrying file I/O.
- [ ] `Application` projects desired hotkey configuration separately from its one confirmed-binding bit; cleanup errors preserve the current bit, registration loss clears it, and shutdown never presents a cleared binding as active.
- [ ] The idempotent Application shutdown path clears the global hotkey, retains any existing dirty-config diagnostic without retrying synchronous file I/O, and then requests native runtime drain; neither UI, host, nor `main` duplicates those operations.
- [ ] Stop retains the runtime through pending drain and releases it only after `Stopped`; shutdown inspects a pre-existing retained Stalled status after finalization/stop initiation instead of waiting for another edge, and runtime teardown finishes before controller/GLFW/COM teardown.
- [ ] Controller-device loss uses only the public host shutdown path and never exposes a replica runtime to `DesktopHost`.
- [ ] Startup/release documentation states that both controller and replica presentation devices require KHR/EXT swapchain-maintenance present fences; absence is a typed unsupported-hardware result and does not silently claim CPU-copy compatibility.
- [ ] Controller surface loss uses ordinary presentation retirement; only confirmed controller Vulkan device loss abandons failed Vulkan fences.
- [ ] Controller acquire is timeout-zero and result-classified with a binary semaphore plus host-polled acquire fence; pending acquire yields a future deadline without submit/present/busy-loop, success/suboptimal alone owns the image and both signals, and every unsuccessful result leaves both supplied sync objects unaffected.
- [ ] Controller submit uses the shared classifier: success retains normal ownership, OOM cleanup waits only for acquire-fence proof before attempting classified maintenance release, device loss uses lost-device teardown, and uncertain submit failure retains every possibly referenced object through proof or non-unwinding abandonment.
- [ ] Controller Vulkan construction requires KHR/EXT swapchain-maintenance present fences; it classifies every present and image-release result, clears acquired ownership only on `VK_SUCCESS`, diverts surface loss, retains uncertain release state under the swapchain through legal destruction, and never reuses an `oldSwapchain` retired by a failed recreation call. Same-surface recreation waits for required releases; completed unconsumed semaphores are destroyed/replaced without reuse.
- [ ] Both GLFW product windows are `GLFW_NO_API`; the existing controller setup callable and overlay adapter reject `CS_OWNDC`/`CS_CLASSDC`, proving the marked GLFW patch before layered Pass-through is offered.
- [ ] Runtime feedback is returned only by the currently owned runtime's no-throw main-thread tick in a fixed eight-slot batch; no callback-held application sink/event queue/epoch exists.
- [ ] Runtime reports all distinct pre-stop lifecycle candidates without portable ranking; Application alone selects one outcome using the stated precedence and applies a selected same-batch recovery start/completion pair causally. Runtime status resets on publication/release and drain warnings have Stalled and Normal edges.
- [ ] `stop_requested`, not the visible lifecycle, gates `Stopped`; every terminal failure of an active runtime arrives as allocation-free `Failed { AppErrorCode::RuntimeStateLost }`, repeated stop-phase exceptions retain a bounded deadline and become drain uncertainty/Stalled, terminal Error survives teardown, and normal stop/replacement/quit use their intended disposition.
- [ ] Mode remains session-only; only hotkey, FPS, and border can mutate persisted config.
- [ ] A controller-emitted command skips the following event wait and is processed on the next loop iteration.
- [ ] Desktop sources obey the application-header allowlist and the `Application` operational API exposes no ports or orchestration helpers.
- [ ] Controller exclusion status is obtained before `Application` construction; overlay status arrives as a nonfatal current-runtime event; both reach `ControllerViewState` without native handles.
- [ ] View projection receives all and only the portable state needed to distinguish no-runtime Error from Error with a retained draining runtime; it receives no runtime/port/service object.
- [ ] Static `PlatformCapabilities` has no GPU bridge field; one `BridgeModeChanged` status projects capability fallback reasons and `InteropOperationFailure` from runtime feedback without a duplicate degraded event.
- [ ] Selected-source color warnings distinguish `HdrDetectedUnsupported` from `UnknownAssumeSdr`; `Sdr` emits no color warning, and the UI performs no DXGI query.
- [ ] Seven manual groups and package audit are recorded without overstating skips.
- [ ] Third-party notices identify the exact GLFW 3.4 version and plainly marked no-`CS_OWNDC` source alteration.
- [ ] No new top-level automated case is added merely to cover another row/branch; case 1 uses six flat groups, case 8 uses seven, table rows are non-Cartesian, and no nested `SECTION` matrix hides behind the fixed 15-case inventory.
- [ ] `IReplicaRuntime::apply/tick` and `Application::step/shutdown` are no-throw boundaries; unexpected Application or controller/UI exceptions select the existing non-unwinding abandon path without releasing possibly live native owners.
- [ ] No benchmark, child-process fault matrix, presenter hierarchy, or generalized service container was added.
- [ ] No commit or push is performed by this documentation-review task.
