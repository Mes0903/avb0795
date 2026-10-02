# Airelinna V1 Implementation Plan

> **For agentic workers:** Execute the four subsystem plans in order. The documents describe implementation work but do not authorize a commit, push, remote change, or history rewrite.

**Goal:** Build the Windows 11 V1 application while preserving strict UI/application/platform boundaries, explicit resource ownership, and a lean verification suite.

**Architecture:** Portable domain and application ports are established first. Windows source, hotkey, capture, overlay, and graphics adapters implement those ports without exposing native resources upward. The application is the only command orchestrator; the controller can neither reach the window nor call through the stack.

**Tech Stack:** C++23, CMake 3.28+, ClangCL preferred, exact GLFW 3.4 with one marked Vulkan-only no-`CS_OWNDC` patch, Dear ImGui, Vulkan 1.3 with KHR/EXT swapchain-maintenance present fences, Windows Graphics Capture, D3D11/DXGI/Win32, Catch2 and CTest.

**Supported-hardware floor:** Both the controller and replica Vulkan devices must expose and enable the `VK_KHR_swapchain_maintenance1` or `VK_EXT_swapchain_maintenance1` present-fence feature. A device without either is unsupported in V1 even when CPU-copy capture is available; this deliberate floor avoids a second, less direct present-retirement implementation.

---

## 1. Execution order

```text
P1 Foundation: domain values, application ports, mailbox/scheduler/config
       -> P2 Windows capture: source catalog, hotkey, WGC frame/job
       -> P3 Overlay/renderer: Win32 window adapter, Vulkan/D3D stack,
                               Windows runtime composition
       -> P4 Application/release: controller, orchestration and packaging
```

- [ ] P1 creates only portable targets and four deterministic tests.
- [ ] P2 adds Windows adapters, three policy/contract tests, and one hardware test.
- [ ] P3 adds overlay/graphics, three deterministic/policy tests, and two hardware tests.
- [ ] P4 adds the controller/application, two deterministic tests, seven grouped manual scenarios, and release packaging.
- [ ] Run the final architecture, test-budget, build, and package gates in this index.

## 2. Normative dependency rules

| Layer | May depend on | Forbidden dependencies |
|---|---|---|
| `core` | C++ standard library | `app`, persistence, `ui`, `platform`, GLFW, ImGui, Win32, D3D, Vulkan |
| `app` | `core`, JSON/filesystem support, portable port declarations | `ui`, concrete Windows adapters, native handles |
| `ui` | `core` view values, `app` commands, Dear ImGui widgets | `platform`, renderer, native GLFW access, direct runtime calls |
| `desktop` shell | `app`, `ui`, controller-only GLFW/Vulkan/ImGui infrastructure | Domain mutation or direct replica-runtime access |
| `platform/windows` | `core`, `app` ports, GLFW native access where required, Win32/WGC/D3D/Vulkan | UI model mutation or domain mutation |
| executable composition root | all concrete construction modules | Business logic beyond construction and process exit handling |

A source-boundary script checks these include rules. It is one architecture gate, not a separate test for every header.

## 3. Authoritative contracts

The design spec is authoritative. Plans use these exact responsibilities:

- `ReplicaSession` and `ApplicationState`: portable state only.
- `AppCommand`: the sole frontend-to-application control path.
- `ControllerViewState`: the sole application-to-frontend state path.
- `DesktopHost`: owns the controller window/renderer, its own presentation retirement, and event loop; it calls only public application steps/view projection.
- `ISourceCatalog`: source discovery only; the Windows implementation creates each exact capture-item identity and installs its sole typed `Closed` observation before publishing a descriptor.
- `IReplicaRuntimeFactory`: create a platform runtime only.
- `IReplicaRuntime`: no-throw transactional `apply`, no-throw main-thread `tick(now)` returning an allocation-free fixed event batch/deadline, and idempotent nonblocking `request_stop()`; `AppError` owns no dynamic text, and the port exposes no native handles or callback-held application sink.
- `IGlobalHotkey`: registration/replacement and command notification only.
- `WindowsReplicaRuntime`: canonical aggregate owner of stable source, active overlay/current capture job/graphics lifetime, and monotonic resource-generation counter.
- `WindowsSourceRegistry`: current-generation owner of each exact `GraphicsCaptureItem` plus its one pre-publication typed `Closed` observation; it never exposes/re-resolves bare native handles or refreshes while a runtime is retained.
- `WindowsCaptureSource`: runtime lease/wake binding and edge cursor over that same item observation across WGC/graphics generations; it never registers a second watcher.
- `WgcCaptureJob`: canonical owner of one frame-pool/session generation, initially closed frame admission, frame callback, mailbox, and fatal/resize latches.
- `WindowsCapturedFrame`: move-only Windows type owning the actual WGC frame-pool checkout.
- `WindowsRenderStack`: canonical owner of Vulkan/D3D devices, timeout-zero result-classified acquire, classified submit records, optional active swapchain/retired set, result-classified present fences/resources, bridge slots and fences for its assigned generation.
- `Win32OverlayAdapter`: validates the patched GLFW class before canonically owning the `HWND` subclass, transactional input-transparency styles/layered attributes, and native interaction policy; `OverlayWindow` alone owns GLFW decoration.
- `WindowsRuntimeFactory` local construction guard: is created disarmed after the heap-stable detached adapter/thunk exists, arms with a no-throw operation immediately after successful subclass attachment, and then protects the not-yet-published overlay/adapter pair through later construction and activation. It performs the same checked detach-or-destroy-with-adapter-alive sequence on failure, then disarms only after successful runtime publication. It is concrete/local cleanup, not a port or service.

The removed contracts must not reappear: generic `CapturedFrame`, generic `RenderFrame`, `void* native_resource`, `shared_ptr<void>` lifetime, `IFrameBridge`, `IVulkanDriver`, or frontend references to window/render objects.

## 4. V1 test budget

The suite has **15 named automated cases**, all expressed as top-level Catch2 `TEST_CASE`s in deterministic, Windows-policy, or hardware executables. Every title begins exactly `Airelinna V1 case NN:` using its two-digit ID, every case carries the `[airelinna-v1]` tag, and only cases 13-15 additionally carry `[hardware]`. Register every executable with `catch_discover_tests(... ADD_TAGS_AS_LABELS)` so those built cases and tags become CTest names/labels. Catch2 `SECTION`s and table-driven rows may cover multiple values without creating a new top-level case.

| ID | Case | Owner plan | Tier |
|---:|---|---|---|
| 1 | Normative Application lifecycle including Error-without-runtime restart and Interrupted replacement, sole portable event arbitration/status reset including a same-batch recovery pair, asynchronous stop, settings/config boundary including shutdown no-retry, stale-SessionId rejection | P4 | deterministic |
| 2 | Newest-only mailbox replacement/close/move destruction | P1 | deterministic |
| 3 | 30/60/144/Unlimited scheduler and missed periods | P1 | deterministic |
| 4 | Config defaults/validation/round trip/atomic-failure cleanup/allowed keys | P1 | deterministic |
| 5 | Typed hotkey validation/serialization | P1 | deterministic |
| 6 | Aspect fit and MAILBOX-to-FIFO policy | P3 | deterministic |
| 7 | Complete controller projection, including runtime-presence/drain-enabled states and warnings | P4 | deterministic |
| 8 | Seven bounded graphics groups: classifiers, representative ownership traces, swapchain/surface-only recovery, source reconfigure, D3/device recovery, bridge selection, and exact retry/no-progress timing boundaries plus repeated-exception drain deadlines/Stalled retention | P3 | deterministic |
| 9 | Windows source eligibility/filtering, current-process exclusion, generation rejection, and SDR/HDR/unknown color classification without native-item doubles | P2 | policy |
| 10 | Windows hotkey replacement rollback, restore-command emission, and WndProc exception containment | P2 | policy |
| 11 | GLFW class compatibility, Win32 opaque layered input-transparency/restoration, no-throw WndProc, ordered partial-factory cleanup, hit-test/DPI/aspect-resize/border geometry and settings rollback | P3 | policy |
| 12 | Closed-admission two-sided pending-arrival/open handshake, shared item-observation/runtime-wake, WGC late/no-throw callback and stop-cleanup failure retention, prepare/check/commit reconfiguration, immutable pool-format validation, edge semantics, and P2-owned frame lifetime | P2 | contract |
| 13 | WGC window/display capture using the catalog-published concrete item, observed closed-before-start rejection without retargeting, and clean stop | P2 | hardware |
| 14 | CPU-copy capture-to-present, nonblocking acquire, resize/resume, present-fence-aware stop, and resource stability | P3 | hardware |
| 15 | GPU interop, real capability-query wiring, and selected-path verification | P3 | hardware |

Do not add compiler-version tests, trivial capability tests, fake-only contract tests, mailbox benchmarks, a test per controller property, or five child-process fault scenarios. The V1 inventory is fixed at 15 (within the user's requested roughly 10–20 range); any new top-level case requires explicit user approval and a corresponding inventory/gate change. Table rows execute in one non-Cartesian loop. Case 8 is capped at seven named `SECTION` groups, and no case may hide a combinatorial matrix behind nested `SECTION`s.

## 5. Requirement traceability

| Requirement | Implementation owner | Evidence |
|---|---|---|
| Text display/window selection | P2 source catalog; P4 controller | cases 7, 9, 13; manual source scenario |
| One active overlay, future session-shaped domain | P1 state; P4 application | case 1 |
| Editable/Pass-through and recovery | P3 patched GLFW class/adapter; P2 hotkey; P4 commands | cases 1, 7, 10, 11; manual interaction scenario |
| Bordered/borderless, drag/resize, aspect fit | P3 | cases 6, 11; manual mixed-DPI scenario |
| 30/60/144/Unlimited without interpolation | P1 scheduler; P4 view | cases 3, 7; manual FPS scenario |
| WGC capture and source interruption from before publication | P2 registry observation/job; P4 application transition | cases 1, 9, 12, 13; manual source scenario |
| GPU path plus bounded CPU fallback | P3 render stack | cases 8, 14, 15 |
| Mandatory present-fence supported-hardware floor for both Vulkan owners | P3/P4 construction | startup/runtime preflight; cases 14, 15 or explicit hardware skip |
| Nonblocking Vulkan acquire plus safe presentation retirement/recreation | P3 replica owner; P4 controller owner | cases 8, 14; manual controller-shutdown scenario |
| Typed graphics recovery | P3 policy and runtime integration | case 8; architecture gate; manual observation only with a supported trigger |
| Nonblocking safe stop and overlay close | P3 runtime; P4 application | cases 1, 8, 14 |
| Hotkey replacement keeps desired configuration separate from confirmed OS binding | P2 adapter; P4 Application projection | cases 1 and 10; manual hotkey scenario |
| Transactional native settings rollback | P3 runtime transaction | cases 1 and 11 |
| Config stores only hotkey/FPS/border | P1/P4 | case 4; schema inspection |
| SDR V1 and HDR/unknown-color warning | P2 catalog producer; P4 projection | cases 7 and 9; manual warning scenario |
| Best-effort self-capture exclusion | P2 helper; P3 overlay invocation/event; P4 controller invocation/projection | manual warning scenario |
| Application-level hotkey/config notices | P4 | cases 1, 7 |
| Privacy and packaging | P4 | manual package audit |
| No frontend stack penetration | all plans | source-boundary gate |
| No V1 Linux/macOS implementation | design/post-V1 only | source and target scan |

## 6. Cross-plan construction and destruction

Construction order:

```text
process/COM setup
-> GLFW
-> stable command queue
-> patched GLFW_NO_API DesktopHost controller window/private Vulkan-ImGui backend with required present fences and class-style guard;
   obtain portable controller CaptureExclusionStatus
-> WindowsSourceRegistry
-> WindowsSourceCatalog creates exact capture items and installs each sole Closed observation before publishing into WindowsSourceRegistry;
   WindowsRuntimeFactory,
   WindowsGlobalHotkey and ConfigStore
-> portable static Windows 11 PlatformCapabilities (no GPU bridge claim)
-> Application(static capabilities, controller CaptureExclusionStatus)
-> on Start: lease the selected pre-created item observation into one stable WindowsCaptureSource and bind its weak runtime wake state
-> overlay GLFW window
-> fully construct a heap-stable detached Win32OverlayAdapter/thunk
-> create one disarmed factory-local cleanup guard for the overlay/adapter pair
-> attach/subclass as the adapter's final fallible action, then arm the guard without throwing
-> WindowsRenderStack(overlay window, required present-fence support)
-> inactive WgcCaptureJob(stable source, render stack's D3D device)
-> complete WindowsReplicaRuntime aggregate
-> transfer overlay/adapter ownership into the local aggregate while the guard remains armed
-> WgcCaptureJob::start_for_publication: final no-throw source check, then StartCapture as the last fallible operation/linearization point
-> no-throw admission open plus pending-arrival wake, runtime publication, then disarm the local guard
```

Shutdown order:

```text
stop UI command intake
-> unregister global hotkey
-> retain an unsaved-settings diagnostic for any earlier command-time save failure; do not retry synchronous config I/O
-> request runtime stop; detach source wake, close/revoke job callback admission/handler and return
-> close the job-owned frame mailbox
-> continue deadline-driven runtime ticks through callback/D3/Vulkan submission/presentation quiescence; every incomplete drain contributes a bounded observation deadline
-> release slot-held WGC checkouts only after D3 completion or confirmed D3 removal
-> destroy WGC job/frame pool
-> destroy WindowsRenderStack and Vulkan surface after ordinary present-fence retirement
-> checked Win32 adapter detachment; on success destroy adapter, then overlay
-> on detachment failure keep adapter/thunk alive, destroy overlay through forwarded WM_NCDESTROY, then destroy adapter
-> quiesce/release the stable source wake state and observation lease
-> runtime emits Stopped; Application releases the empty runtime
-> Application sets ExitDisposition::Clean
-> retire DesktopHost's own pending controller presents
-> DesktopHost::run returns with its Application reference no longer in use
-> destroy Application
-> retire registry item observations and destroy Windows services in reverse construction order
-> destroy DesktopHost controller graphics/window/ImGui
-> destroy command queue
-> terminate GLFW/COM
```

No plan may reverse these orders or keep references beyond an owner's destruction.
If a quit/host-fatal drain instead reaches the bounded Stalled threshold, the already-finalized hotkey plus retained config-dirty diagnostic remain, Application retains the runtime and selects `ExitDisposition::AbandonProcess`, and `main` terminates without stack unwinding; none of the normal native-destruction steps above are claimed to have run.

## 7. CMake and implementation sequencing rules

- A target receives a source only after that file is created in the same task.
- Windows-only targets and `target_sources` calls are entirely inside `if(WIN32)`.
- Include roots use `${PROJECT_SOURCE_DIR}/include`; no `/include` absolute path.
- The first runnable vertical slice is CPU-copy WGC-to-present. GPU interop is added afterward as an internal optimization.
- The executable is a console target while using `main`; switch to `WIN32` only together with a correct `wWinMain` entry and diagnostic strategy.
- No task intentionally creates a broken CMake graph merely to demonstrate a failing test. A red test may fail after the target configures and compiles its test harness.
- Plan snippets must name every function they call; no prose-only `run_integration_scenario` or placeholder implementation.

## 8. Final architecture gates

- [ ] `core`, `app`, and `ui` contain no Win32/D3D/Vulkan/GLFW-native includes.
- [ ] `ui` includes only the two application contract headers for `ControllerViewState` and `AppCommand`, links no `airelinna_app`, and cannot call `Application`, `IReplicaRuntime`, `OverlayWindow`, or `WindowsRenderStack`.
- [ ] `desktop` owns controller graphics/event-loop infrastructure, obeys the exact application-header allowlist, and cannot access a port/runtime or mutate domain state directly.
- [ ] The single boundary module checks both source/include rules and configure-time target source/direct-link allowlists for portable, UI, DesktopHost, Windows/render and executable targets.
- [ ] Native frame and synchronization types exist only in Windows/graphics implementation files.
- [ ] WGC frame admission remains closed until the complete runtime aggregate exists; a matching early event records one pending bit without pulling, the final no-throw source check precedes `StartCapture`, successful `StartCapture` is the last fallible operation/publication linearization point, and only no-throw admission/pending wake/ownership transfer follows it. The first published tick services the bounded pending pull; a later source close is the first normal `SourceLost` edge.
- [ ] The heap-stable adapter/thunk is complete and the concrete local guard already exists before WndProc attachment; successful attachment is followed immediately by a no-throw arm. Any later pre-publication failure attempts checked detachment and, on restoration failure, keeps adapter/thunk state alive while destroying the overlay through forwarded `WM_NCDESTROY`; ordinary reverse RAII alone is not claimed sufficient.
- [ ] WGC delegates plus hotkey/overlay WndProcs are `noexcept`, contain all C++/projection/allocation exceptions, and never invoke forbidden higher/lower layers; WGC delegates capture only weak typed state and use one newest-only mailbox, bounded signal latches, plus one current-epoch pending-arrival bit rather than an event queue.
- [ ] Terminal job/source `request_stop()` and recoverable `begin_reconfigure/prepare_reconfigure/commit_reconfigure` initiate without waiting; prepare installs the new handler with admission still closed, callback set-then-recheck and opener store-then-read cover both pending-arrival races without clearing the bit, the final source check precedes the no-throw commit/wake, and the next tick atomically claims one pool-buffer-count-bounded pull. Job stop contains native revoke/close failure while retaining uncertain state; late/stale callbacks cannot reach destroyed/reused targets.
- [ ] P3 consumes WGC only through one source edge, one job `drain_signals()`, conditional `take_latest()`, explicit quiescence/reconfiguration, and terminal stop operations; callback state/tokens are never exposed.
- [ ] Runtime feedback is returned synchronously by the current runtime's no-throw main-thread tick in eight fixed optional slots; callbacks retain state only inside `WgcCaptureJob` and cannot reach `Application`.
- [ ] Runtime translates and reports all distinct pre-stop lifecycle candidates without ranking them; Application is the sole owner of the stated order-independent portable arbitration. A selected same-batch recovery start/completion pair is applied causally in that order rather than dropping completion, while later recovery facts self-transition in Interrupted so native repair cannot hide source disconnection. Runtime stop phase permits only a lone safe `Stopped`, which remains the only release permission.
- [ ] Application's private `stop_requested` phase accepts `Stopped` even while visible lifecycle remains Error, then preserves Error or performs normal/replacement/quit disposition.
- [ ] Runtime-scoped bridge/exclusion/drain feedback resets on publication/release, drain status has both Stalled and Normal edges, and recovery lifecycle remains domain-owned.
- [ ] The event loop receives the earliest runtime/graphics/incomplete-drain/application-immediate deadline and never polls pending CPU-copy work in a busy loop, sleeps past the fixed 50 ms drain poll or 10-second no-progress boundary, or sleeps after post-tick work. Graphics reconstruction uses exactly three attempts at 0/100/500 ms and resets only after a successful replacement present; replica surface/swapchain, replica device, and controller counters remain with `WindowsRenderStack`, `WindowsReplicaRuntime`, and `DesktopHost` respectively, and only the runtime owns portable Stalled/Normal status.
- [ ] Both Vulkan owners use timeout-zero acquire with a binary semaphore plus host-polled acquire fence and the same pure acquire classifier; pending returns a future deadline with no submit/present, only success/suboptimal owns the acquired image/signals, and every unsuccessful result leaves both supplied sync objects unaffected.
- [ ] Both Vulkan owners use the same pure submit classifier; OOM is treated as guaranteed not-enqueued and waits only for acquire-fence proof before attempting classified maintenance release, device loss uses lost-device teardown, while unknown submit state retains every possibly referenced object until fence proof or non-unwinding process abandonment.
- [ ] Both concrete Vulkan presentation owners enforce the documented V1 supported-hardware floor by requiring KHR/EXT swapchain-maintenance present fences, share pure present and image-release classifiers, retain enqueued/possibly enqueued present resources until signaled, and never substitute submission fences or `WaitIdle`.
- [ ] A known not-enqueued present releases only its unused present fence; after submission completion, maintenance release clears the acquired image only on `VK_SUCCESS`, routes surface loss to surface teardown, and retains uncertain release state under the swapchain through legal destruction. Completed unconsumed semaphores are destroyed/replaced, never reused.
- [ ] Both owners model optional active plus retired swapchains; same-surface creation waits for all required image releases, while failed creation with `oldSwapchain` leaves active empty and retries with `VK_NULL_HANDLE` without reusing the retired handle.
- [ ] A command emitted during controller rendering skips the next event wait.
- [ ] Exact GLFW 3.4 carries the marked one-line main-class no-`CS_OWNDC` patch, both windows are `GLFW_NO_API`, CMake rejects patch drift, and both actual window setup paths reject `CS_OWNDC`/`CS_CLASSDC` before layered mode.
- [ ] Pass-through is adapter-owned: it sets `WS_EX_LAYERED | WS_EX_TRANSPARENT`, establishes verified opaque alpha 255, and restores the exact prior layered/style snapshot; both border modes are manually verified against another process, and GLFW mouse pass-through/`HTTRANSPARENT` are not sufficient.
- [ ] WndProc detachment is checked in both published-runtime and partial-factory cleanup; restoration failure keeps adapter/thunk state alive through forwarded `WM_NCDESTROY` before adapter destruction.
- [ ] Controller exclusion status enters `Application` at construction; overlay exclusion status returns through the current runtime tick; neither path exposes a native handle upward.
- [ ] WGC checkouts outlive their D3 source copy; old allocation rebuild also waits every Vulkan reference to old slots before frame-pool `Recreate`, installs the new handler with admission closed, checks source openness, and then commits admission.
- [ ] Replica `SurfaceLost` is handled inside `WindowsRenderStack` by retiring/recreating only surface/swapchain-dependent Vulkan state while WGC, D3, slots, and generation remain live. `VulkanDeviceLost` does not imply D3 cancellation and alone permits lost-Vulkan-fence abandonment; Vulkan submission waits only an already-observed D3 fence value, so confirmed `D3DeviceRemoved` cancels only pre-submit copies and cannot strand a healthy Vulkan external wait.
- [ ] Catalog publication occurs only after installing one typed sticky `Closed` observation on the exact item; runtime leases that same state/COM identity without a second watcher or bare-handle re-resolution, and the lease outlives all WGC job/graphics generations.
- [ ] `WindowsReplicaRuntime` survives render-stack replacement as the sole monotonic generation owner.
- [ ] One class canonically owns each window, WndProc subclass, stable source/`Closed` watcher, WGC session/mailbox, graphics stack, slot ring, hotkey registration, and config store.
- [ ] P2 case 12 stops at source/job behavior; P3 case 8 owns only the seven bounded graphics groups, while Application case 1 alone owns portable lifecycle arbitration.
- [ ] Device generation invalidates old frames and rebuilds both graphics and capture resources.
- [ ] `request_stop()` retains the runtime until `Stopped`; `RuntimeDrainStatus` covers callback/job cleanup and graphics retirement, every incomplete fact—including repeated exceptions during stop—contributes a bounded next-wake/stall-observation deadline, and after hotkey cleanup, dirty-config diagnostic retention, and stop initiation Application checks both an already-retained Stalled status and future Stalled edges before selecting `AbandonProcess` without fabricating `Stopped`.
- [ ] `IReplicaRuntime::apply/tick`, `Application::step/shutdown`, and native callback boundaries contain exceptions. `AppError` plus runtime batches are allocation-free nothrow-copy/move values; pre-stop runtime exceptions report fixed `RuntimeStateLost`, while stop-phase exceptions retain a bounded deadline and become drain uncertainty/Stalled. Unexpected host/controller failures use the existing non-unwinding abandon path while all possibly live native owners remain retained; no new exception service is introduced.
- [ ] Every active-runtime terminal capture/graphics/recovery failure maps to the fixed `AppErrorCode::RuntimeStateLost`; no local `DeviceLost` error code appears.
- [ ] Platform capabilities contain no pre-runtime GPU bridge claim; one `BridgeModeChanged` value owns CPU/GPU status, with capability reasons distinct from `InteropOperationFailure` after capability success.
- [ ] Mode is session-only; persisted config remains exactly version/hotkey/FPS/border.
- [ ] Effective process DPI awareness is verified as `PER_MONITOR_AWARE_V2` before either GLFW window is created.
- [ ] The entire CTest inventory—not only already-labelled entries—contains exactly 15 Airelinna cases; all carry `airelinna-v1`, and only IDs 13–15 carry `hardware`, unless the user approves changing the inventory.
- [ ] No unused Linux/macOS classes, generalized plugin system, telemetry backend, virtual filesystem, or mock hierarchy was added.

## 9. Final verification commands

Run from a Windows Developer PowerShell after implementation exists:

```powershell
cmake -S . -B build/clang-release -G 'Visual Studio 17 2022' -T ClangCL -DAIRELINNA_BUILD_TESTS=ON -DAIRELINNA_ENABLE_WINDOWS_INTEGRATION=ON
cmake --build build/clang-release --config Release
ctest --test-dir build/clang-release -C Release --output-on-failure -LE hardware
ctest --test-dir build/clang-release -C Release --output-on-failure -L hardware
```

Run the source-boundary target and inspect the built, CTest-registered inventory rather than counting source text:

```powershell
cmake --build build/clang-release --config Release --target verify_source_boundaries
& .\cmake\VerifyTestInventory.ps1 -BuildDir build/clang-release -Config Release
```

`VerifyTestInventory.ps1` consumes the **entire** `ctest --show-only=json-v1` test array after all Airelinna test executables are built and dependency tests are disabled. It rejects any discovered test lacking the `airelinna-v1` label, any name outside `Airelinna V1 case NN:`, duplicate/missing IDs, any total other than 15, or a `hardware` ID set other than exactly 13, 14, and 15. It validates discovered commands/names/labels rather than scanning source comments, so an extra unlabelled Catch2 case cannot hide outside the count.

Build the package only after automated and named Windows manual checks have recorded honest pass/skip evidence.

## 10. Documentation and Git gate

- [ ] Spec and plans remain uncommitted unless the user later explicitly authorizes committing them.
- [ ] This review task does not commit test files, create a remote, push, amend, reset, or rewrite history.
- [ ] `git diff --check` checks tracked changes, and a separate read-only trailing-whitespace scan explicitly checks all six untracked/working-tree spec and plan paths without staging them.
- [ ] Placeholder, mojibake, signature-drift, dependency-direction, and test-count scans pass.

Use this read-only documentation whitespace gate in addition to `git diff --check`:

```powershell
$docs = Get-ChildItem docs/superpowers/specs,docs/superpowers/plans -File -Filter '*.md'
$badWhitespace = $docs | Select-String -Pattern '[ \t]+$'
if ($badWhitespace) { $badWhitespace; throw 'Trailing whitespace in spec/plan docs' }
git diff --check
```
