# Airelinna V1 Windows Capture Implementation Plan

> **For agentic workers:** Run after P1. This plan adds Windows adapters only and does not authorize a commit or push.

**Goal:** Implement Windows source discovery, typed global hotkeys, WGC capture ownership, and callback quiescence without exposing native resources to the application or UI.

**Architecture:** Concrete Windows classes implement source and hotkey ports plus capture primitives. WGC callbacks create move-only Windows frames and submit them to a newest-only mailbox. P3 composes these primitives with overlay/graphics into the canonical Windows runtime.

**Tech Stack:** C++23, Win32, C++/WinRT Windows Graphics Capture, D3D11/DXGI.

---

## 1. Files and responsibilities

| Path | Responsibility |
|---|---|
| `platform/windows/windows_source_registry.*` | Current-generation mapping from opaque IDs to pre-created items plus their sole pre-publication `Closed` observation states |
| `platform/windows/windows_source_catalog.*` | Display/window enumeration, eligibility filtering, capture-item creation and source descriptors |
| `platform/windows/windows_capture_source.*` | Runtime lease/wake binding and edge consumption over one registry item observation |
| `platform/windows/windows_global_hotkey.*` | Hidden message window and transactional registration |
| `platform/windows/windows_captured_frame.hpp` | Move-only COM-owning WGC frame |
| `platform/windows/wgc_capture_job.*` | One D3/frame-pool generation, frame callback/mailbox and nonblocking quiescence |
| `platform/windows/capture_exclusion.*` | Best-effort own-window exclusion result |
| `tests/windows/windows_policy_test.cpp` | Named cases 9, 10, and 12 |
| `tests/windows/wgc_hardware_test.cpp` | Named case 13 |

Do not create `IWindowsInteropApi`, `IHotkeyApi`, a generic capture-frame interface, or a second application state machine.

## 2. Task 1: Create Windows targets only after sources exist

- [ ] Under `if(WIN32)`, create `airelinna_windows` with the first source-catalog file.
- [ ] Add subsequent files to the target in the task that creates them.
- [ ] Link the P1-created `airelinna_core` and `airelinna_app` targets plus `user32`, `d3d11`, `dxgi`, and `windowsapp` only where needed; do not reference a target before its creating task has run.
- [ ] Define `NOMINMAX` and `WIN32_LEAN_AND_MEAN` privately.
- [ ] Create one `airelinna_windows_policy_tests` executable for cases 9, 10, and 12. Register it with `catch_discover_tests(... ADD_TAGS_AS_LABELS)`; do not create a new executable per policy.
- [ ] Register the hardware executable with `catch_discover_tests(... ADD_TAGS_AS_LABELS SKIP_IS_FAILURE PROPERTIES SKIP_RETURN_CODE 77)` only when `AIRELINNA_ENABLE_WINDOWS_INTEGRATION` is on. `SKIP_IS_FAILURE` prevents Catch2's helper from appending its default skip code 4, leaving the explicit CTest code 77 authoritative. Its custom main bypasses environment preflight only for Catch2 list/discovery arguments, performs no capture then, and otherwise returns 77 before entering the requested case when hardware preflight is unavailable.

Expected: non-Windows configuration does not mention or evaluate Windows source paths; Windows configuration never references a not-yet-created file.

## 3. Task 2: Implement source discovery

`main` canonically owns one `WindowsSourceRegistry` and injects it by reference into both `WindowsSourceCatalog` and the P3 `WindowsRuntimeFactory`. Catalog refresh replaces the registry snapshot and increments its generation. `WindowsSourceCatalog` implements `ISourceCatalog` and returns portable `SourceDescriptor` values whose IDs refer to that snapshot. After eligibility filtering, the catalog calls `IGraphicsCaptureItemInterop::CreateForWindow` or `CreateForMonitor` while the native candidate is still the object it enumerated. It immediately creates one concrete `shared_ptr<ItemObservationState>` containing that exact strong item, sole `Closed` token, sticky closed bit/edge and callback admission; the delegate captures only a `weak_ptr` to this state. The descriptor is published only after handler registration succeeds. A candidate whose item/handler cannot be created is omitted rather than publishing a descriptor that could miss closure or later retarget; failure of enumeration itself returns `AppErrorCode::SourceCatalogFailure`. `ItemObservationState` is a private Windows value/state, not an exported interface or generic observer framework.

The catalog also owns `SourceDescriptor::color_support` production through one private concrete DXGI helper. On each refresh it creates a current factory, maps a display candidate to its `HMONITOR` output and a window candidate to the output with greatest rectangle overlap, then queries `IDXGIOutput6::GetDesc1`. Active `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` becomes `HdrDetectedUnsupported`; `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709` becomes `Sdr`; every other successful color-space value, missing output/interface, or query failure becomes `UnknownAssumeSdr`. This plan publishes metadata only: P4 projects distinct visible warnings for both non-SDR values, while this adapter never emits UI text. The snapshot may change only after Refresh; the limitation is documented rather than adding a display-event service. The helper neither affects capture identity nor introduces another port/interface.

### Display sources

Use display-monitor enumeration to produce one descriptor per active desktop display. The source ID is opaque outside the registry; portable code does not parse or cast it.

### Window sources

Enumerate top-level windows and apply one policy function that rejects:

- invisible or cloaked windows;
- zero-area windows;
- tool windows not intended for user selection;
- every top-level window whose `GetWindowThreadProcessId` PID equals the current Airelinna process;
- windows without a usable title after the selected fallback-label rule.

Enumeration is a snapshot. Source IDs remain valid only for their registry generation. Refresh is accepted only when no runtime is retained; it closes observation admission, revokes each sole `Closed` token, replaces the snapshot and drops registry references. Stale IDs return `AppErrorCode::SourceUnavailable` without parsing or dereferencing a stale `HWND`/monitor handle. `Application` clears a selected row absent from the new generation. A source lease may share an observation while a runtime drains, so the no-runtime refresh gate prevents invalidating an active source.

At runtime construction, `WindowsRuntimeFactory` resolves the selected current-generation ID exactly once and gives `WindowsCaptureSource` a strong lease on the **same pre-created observation/COM identity**. It never calls `CreateForWindow`/`CreateForMonitor` again, never performs a bare-handle lookup, and never registers a second `Closed` handler. The source binds a weak runtime wake state, then rejects an already-sticky close before capture-session construction. It retains that lease and consumes its edges throughout the `WindowsReplicaRuntime` lifetime. Closure at any time after descriptor publication is therefore remembered. The source survives WGC job/graphics generations and exposes no native identity to `Application` or UI.

### Test case 9

One table-driven case tests the pure eligibility policy with visible, invisible, cloaked, zero-size, tool, current-process-owned, and normal candidates; a value-level registry-generation row rejects one stale ID; and four value rows map active HDR, active SDR, a successful unrecognised color space, and failed/missing output queries to the exact `ColorSupport` values. Do not template item creation/forwarding, fake COM identity, or add a native API interface solely for this test. Exact concrete-item continuity, pre-publication `Closed` observation, and non-retargeting remain structural production ownership requirements and are exercised with a real consented item in hardware case 13 when available. Case 9 does not mock `EnumWindows`/DXGI or split each flag into another top-level case.

## 4. Task 3: Implement typed global hotkeys

`WindowsGlobalHotkey` implements `IGlobalHotkey` and canonically owns:

- one hidden message-only `HWND`;
- one fixed primary ID and one fixed probe ID, with known/uncertain registration state for each;
- the optional currently confirmed active typed `HotkeyChord`;
- a callback that enqueues session-agnostic `RestoreActiveReplicaEditable` as an `AppCommand`.

The hotkey callback never receives an overlay pointer and never changes a window directly.

Map validated modifiers/key codes explicitly to `RegisterHotKey`; do not parse the last character of a string. Replacement is transactional:

1. if equal to the confirmed current chord and no cleanup is pending, succeed without OS work;
2. before any probe, retry `UnregisterHotKey` for **every** cleanup-pending/uncertain fixed ID, including primary and probe; do not probe or reuse either uncertain ID while any cleanup remains unverified, ignore `WM_HOTKEY` from uncertain IDs, and return `AppErrorCode::HotkeyCleanupFailure` while preserving the inactive/unconfirmed primary state;
3. probe the requested chord with the fixed probe ID;
4. unregister the probe and check the result; on failure mark that ID cleanup-pending, keep the old primary untouched, ignore probe-ID messages, and return `AppErrorCode::HotkeyCleanupFailure`;
5. unregister the old primary and check the result before any new primary registration; on failure do not proceed, mark the primary ID cleanup-pending/unconfirmed, clear the confirmed-active chord, and return `AppErrorCode::HotkeyRegistrationLost` because Win32 provides no query that proves whether the old binding remains;
6. register the new primary;
7. if the final registration races/fails, attempt to restore the old primary and return `AppErrorCode::HotkeyConflict` when restoration succeeds;
8. if rollback also fails, clear the confirmed-active state and return `AppErrorCode::HotkeyRegistrationLost` so the controller does not claim the old hotkey is active;
9. return success to `Application`; the Windows adapter never reads or writes config, and only `Application` persists the accepted chord.

For testability, inject two small registration callables into an internal transaction helper. Do not introduce an exported virtual Win32 API hierarchy.

### Test case 10

One case covers successful first registration, successful replacement, probe conflict preserving the old binding, probe-unregister failure retaining old primary plus pending cleanup, old-primary-unregister failure reporting unconfirmed/inactive state without attempting the new registration, a second replacement retrying primary cleanup **before** probing and refusing reuse while cleanup is still uncertain, final-registration race restoring the old binding, rollback failure reporting no active binding, ignored non-primary/uncertain-ID messages, `WM_HOTKEY` from the confirmed primary emitting exactly `RestoreActiveReplicaEditable` with no session data, and a throwing command sink being contained by the production `noexcept` WndProc while dropping only that one restore request. Use sections or table rows; do not create another top-level case.

The hidden-window WndProc is `noexcept`. It catches every exception from invoking the stable command sink (including allocation failure) and drops that one restore request; it never calls `Application`, retries from inside the callback, or lets an exception cross Win32. `clear()` makes one best-effort unregister attempt for both fixed IDs, clears confirmed state, and records diagnostics; process/thread termination remains the final OS cleanup boundary. The hidden window is created before registration and destroyed only after that call during shutdown.

## 5. Task 4: Define owning Windows frames

`WindowsCapturedFrame` is a final move-only type under `platform/windows` containing:

- sequence number;
- runtime-owned resource generation, frame-pool allocation extent, valid `ContentSize`, and the immutable expected pool-format tag;
- capture timestamp;
- the agile `winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame` checkout itself.

Construction receives the WinRT frame by value and moves it. Copy construction/assignment are deleted. The destructor checks the frame back into its pool exactly once through a no-throw close path. A temporary `ComPtr<ID3D11Texture2D>` may be obtained from `Surface` only while this owning wrapper remains alive; retaining the texture alone does not retain the WGC checkout. There is no `Detach`, manual `AddRef`, `void*`, or type-erased cleanup token.

The frame type never appears in P1 headers, `ControllerViewState`, `RuntimeEvent`, or application commands.

## 6. Task 5: Implement WGC job lifetime

The registry's `ItemObservationState` canonically owns the stable capture item, sole source-`Closed` token, sticky closed bit/edge and one weak-callback admission/counter from before publication. Its callback can only mutate that state and lock an optional weak runtime wake state. `WindowsCaptureSource` shares the observation, owns the runtime edge cursor plus strong wake state, and binds/unbinds that wake state without registering an event. The observation remains active across frame-pool reconfiguration, complete graphics/WGC job replacement, and ordinary source-lease replacement within the same registry snapshot. The source's narrow Windows-local operations remain `consume_closed_edge()`, `is_closed()`, `request_stop()`, and `callbacks_quiescent()`; here stop/quiescence applies to its runtime wake binding, not the registry-owned observation token.

`WgcCaptureJob` owns one free-threaded frame pool, capture session, borrowed D3D device reference, `FrameArrived` token, and one strong `shared_ptr<FrameCallbackState>`. It stores no second application-level `GraphicsCaptureItem`: construction temporarily borrows the exact item from the runtime-owned `WindowsCaptureSource` only to create the capture session, while the source/observation outlives the job and remains the sole explicit item owner. That job state canonically owns the newest-only checked-out-frame mailbox, fixed fatal/resize latches, wake callable, frame-registration epoch/admission counter, and one current-epoch atomic `arrival_pending` bit. Initial frame admission is closed and remains closed through complete runtime-aggregate construction. Delegates capture only a `weak_ptr` to their typed source or frame state, never `this`; neither class is a generic callback framework.

### Construction

- Receive the runtime-owned `WindowsCaptureSource`, borrowed D3D device whose owner outlives the job, resource generation, explicit bounded pool-buffer count, and wake-main-loop callable. P3 supplies destination-slot count plus two for callback/local and mailbox checkout headroom.
- Borrow the source's already-resolved capture item only for the `CreateCaptureSession` call; create the Direct3D device wrapper, free-threaded frame pool with that buffer count, closed-admission `FrameArrived` handler, and capture session using local RAII values, then retain no separate item member in the job.
- Keep `StartCapture` out of the public constructor. P3 first moves the inactive job into the otherwise complete local `WindowsReplicaRuntime`, then calls the concrete Windows-local `WgcCaptureJob::start_for_publication()`. This one-shot factory-only method performs the final no-throw sticky source-open check first, rejects an observed closure, and then catches/translates `StartCapture` as the final fallible native step and publication linearization point. Success opens the existing frame-admission epoch with a no-throw store, then reads without clearing `arrival_pending` and wakes when set; no allocation, callback registration, owner construction, fallible check, or other reject-publication branch remains afterward, and returning the runtime is a no-throw `unique_ptr` transfer.
- A matching callback that observes activation closed sets `arrival_pending`, re-reads admission, wakes if opening won the race, and returns before `TryGetNextFrame`; a stale epoch or terminally closed job returns without changing it. The opener's store-then-read and callback's set-then-recheck form a two-sided handshake; only the next admitted main-thread service exchanges the bit to false. Therefore an observed pre-start closure or a `StartCapture` failure leaves no checked-out frame and unwinds the inactive/not-successfully-started job without an unpublished tick loop. After successful start, the first published tick services any pending arrival, so the event that stored a static source's first frame cannot be lost in either check/open interleaving. A close linearized after the final pre-start check—even between that check and successful `StartCapture`—is consumed after publication as the first normal source-lost edge; the factory never rejects a successfully started capture into synchronous teardown.
- Translate consent/access denial to `AppErrorCode::PermissionDenied`, a closed/stale source to `SourceUnavailable`, and other construction API failures to `RuntimeConstructionFailure`; release partial objects through RAII while admission remains closed.

Each `FrameArrived` registration captures its immutable frame epoch. Its `noexcept` callback first locks the weak frame state and validates that epoch. A stale epoch or terminally closed job returns without touching state. If the matching epoch is temporarily closed only for activation/reconfiguration, it sets the atomic `arrival_pending`, re-reads admission and performs a no-throw wake if it is now open, then returns before `TryGetNextFrame`; otherwise it enters the frame-epoch admission guard. The item `Closed` handler is independently a `noexcept` trampoline: it locks the weak observation state, enters its admission guard, sets sticky state, then may lock the current weak runtime wake state; guard failure touches nothing. Both wake callables and guard-exit paths are no-throw. This remains safe when an asynchronous WinRT invocation was already in flight before handler revocation or runtime-wake detachment.

Expose only these concrete Windows-local consumer operations to P3:

```cpp
std::optional<WindowsCapturedFrame> take_latest();
CaptureSignalBatch drain_signals(); // atomically consume/clear fatal/resize edges
void service_pending_arrival() noexcept; // main-thread, current admitted epoch only
std::expected<void, core::AppError> start_for_publication(); // one-shot factory-only activation
std::expected<void, core::AppError> begin_reconfigure(); // initiate; never wait
bool frame_callbacks_quiescent() const noexcept;
std::expected<void, core::AppError> prepare_reconfigure(
    core::Extent allocation, std::uint64_t generation); // admission remains closed
void commit_reconfigure() noexcept; // open the prepared epoch
void request_stop() noexcept; // initiate terminal stop; never wait
bool stop_quiescent() const noexcept; // mailbox closed, callbacks exited, native stop succeeded
```

`CaptureSignalBatch` is a fixed value containing the already-coalesced fatal/resize outcome and metadata. `drain_signals()` clears notification edges but always reports current sticky fatal state. The runtime merges it with `WindowsCaptureSource::consume_closed_edge()`/`is_closed()` using `FatalCaptureError > SourceLost > Resize`. Those source operations delegate to the shared observation through the runtime cursor; no consumer operation exposes observation/callback state, mutexes, mailbox internals, event tokens, or a generic queue/interface.

### Frame callback

The `FrameArrived` callback performs only:

1. lock `CallbackState` and validate the immutable frame epoch;
2. for a matching activation/reconfiguration epoch observed with closed admission, set `arrival_pending`, re-read admission and wake if it is now open, then return; reject stale/terminal epochs without changing it;
3. otherwise enter the guard that increments the frame in-flight count for the admitted epoch and call `TryGetNextFrame`;
4. read allocation/content metadata, validate non-zero/sane dimensions, and verify that the surface still matches the job generation's fixed pool format;
5. move the owning WinRT frame into `WindowsCapturedFrame` and the job-owned newest-only mailbox;
6. latch only bounded fatal/resize state when needed;
7. wake the main loop only when the mailbox became ready or a previously clear signal bit was set; plain replacement relies on the already-published scheduler deadline;
8. leave the guard and wake the main loop when a reconfigure/stop quiescence transition completes.

The complete trampoline and `service_pending_arrival()` are `noexcept` and use the same contained intake helper around `TryGetNextFrame`, metadata reads, owning-wrapper construction, and mailbox handoff. The main-thread service first verifies the current epoch is admitted, atomically exchanges its pending bit to false, and drains at most the configured frame-pool buffer count only when it claimed true, retaining the newest checkout; it never waits for another frame. Neither admission opener clears the bit. `winrt::hresult_error` records its code; `std::bad_alloc` records the fixed out-of-memory category/code; `catch (...)` records a fixed unknown-callback category/code. The existing `CaptureSignalBatch` stores only this predeclared category and integer native code—never exception text or an allocated error object—and the first newly latched fatal edge invokes the same no-throw wake. Any local frame remains RAII-owned through the catch path, and the admission guard always exits.

It does not call `CopyResource`, map memory, import Vulkan memory, allocate Vulkan objects, render, update domain state, or block.

A `ContentSize` change is reported as resize metadata only. The pixel format supplied to frame-pool creation/recreation is immutable for the job generation; an observed surface-format mismatch latches the existing fatal capture category rather than pretending the source requested a format resize. Job signals are fixed latches; resize metadata is retained only while no fatal condition is latched. P3 drains one job batch and one source-closed edge per tick, applies `FatalCaptureError > SourceLost > Resize`, and never recreates a closed/failed source.

The P3 main-thread runtime owns the full barrier through two concrete job operations:

1. `begin_reconfigure()` closes frame admission, advances the frame-registration epoch, revokes `FrameArrived`, marks reconfiguration pending, and returns without waiting. An already-entered callback may finish into the still-owned mailbox; its final guard exit wakes the loop.
2. Later ticks first wait for `frame_callbacks_quiescent()`, then consume/destroy the final mailbox checkout, then wait for P3's complete `render_allocation_idle()` predicate. `prepare_reconfigure(allocation, generation)` requires callback quiescence, an empty mailbox, and render-allocation idleness; it invokes frame-pool `Recreate` with the job's fixed format, updates frame-tag metadata, and installs a new-epoch handler while admission remains closed.
3. `WindowsReplicaRuntime` rejects sticky fatal state, calls `prepare_reconfigure`, then performs the final source `is_closed()` check. Failure requests ordinary job stop while admission is still closed, discards pending arrival during terminal cleanup, and returns typed `SourceUnavailable`. Success immediately calls allocation-free `commit_reconfigure() noexcept`, which stores the prepared epoch admitted before reading the bit without clearing it and wakes when set; the closed-observing callback performs the complementary set-then-recheck wake. The next tick calls `service_pending_arrival()` before `drain_signals()`. A closure not observed by that final check is consumed as the next normal `RuntimeEvent::SourceLost`.

Failure leaves frame admission closed and is translated by the runtime. Recreation is never mixed with rendering, an entered frame callback, an old-epoch delegate, or an old checked-out frame. The runtime-owned source watcher remains active throughout.

### Stop and quiescence

`request_stop()` is the job's terminal main-thread initiation, is idempotent, `noexcept`, and returns without waiting:

1. atomically set terminal stopping, close frame admission, and advance the frame-registration epoch;
2. close the mailbox immediately so an already-entered callback can only receive `Closed` and release its checkout;
3. attempt `FrameArrived` revocation inside complete C++/WinRT exception/HRESULT containment;
4. attempt capture-session close under the same containment;
5. return while retaining frame pool, callback state, wake callable, and any entered callback.

An entered frame callback's final guard exit wakes the loop. A native revoke/close exception never escapes `request_stop()`: the job stores a fixed cleanup category plus numeric HRESULT-style diagnostic, retains its frame pool/session/token/callback state, and keeps `stop_quiescent()` false rather than claiming teardown is safe. Later runtime ticks observe that predicate; only after native stop succeeded, every entered callback exited, and every render-held checkout is safe may the runtime destroy the job/frame pool. P3 maps a retained cleanup failure to terminal `RuntimeStateLost` when stopping a job for source loss or graphics recovery before runtime stop; once runtime stop is already active, the fact instead participates in the existing bounded whole-runtime drain observation and can become `RuntimeDrainStatus::Stalled`. It creates no new event or cleanup service. `WindowsCaptureSource::request_stop()` independently disables its runtime wake state and returns; ticks retain that small wake state until any callback that already locked it exits, while the observation continues to own the item/token for the registry snapshot. Registry refresh/destruction occurs only with no runtime, closes observation admission, revokes the token and drops its reference; an entered delegate is self-contained and cannot reach a destroyed registry/source. Published-object destructors perform no wait.

### Test case 12

Use controlled callback entry/exit hooks and a move-counted fake frame checkout around the same guard/mailbox logic. Case 12 has six flat groups, with table rows inside a group and no nested `SECTION` cross-product:

1. observation/source-wake lifetime: sticky close before binding, one watcher, safe weak late delegates, and job/source-lease replacement;
2. publication: both forced interleavings of callback set/recheck and opener store/read prove one pending bit and wake without `TryGetNextFrame`, the final sticky check precedes `StartCapture`, pre-start failure destroys the unpublished aggregate without a checked-out frame/drain loop, successful publication services that pending first/static frame, and a close after the check becomes the first published `SourceLost` edge rather than a rejected active session;
3. callback containment: representative projection, allocation, unknown, and immutable-format-mismatch failures latch one fixed fatal edge, wake once, release local ownership, and exit the guard;
4. stop/reconfigure quiescence: entered callbacks, stale epochs, prepare-with-admission-closed, both current-epoch arrival/commit interleavings followed by one bounded service, final source check, and one representative revoke/close failure retaining the job with `stop_quiescent() == false`;
5. edge/mailbox behavior: sticky state remains observable while notification edges consume once and newest-only transfer destroys replaced checkouts outside the lock;
6. owner order: the frame-pool owner outlives callback-local, mailbox, and render-held checkouts through normal, reconfigure, and terminal-stop paths.

Do not split these into a test per atomic operation.

Case 12 stops at P2-owned source/job contracts. P3 case 8 owns one representative source-edge/job-signal merge, render-slot checkout lifetime, and Vulkan/D3 device-loss cleanup; surface-only recovery stays inside `WindowsRenderStack`. P2 must build and pass independently before those types exist.

## 7. Task 6: Add the WGC hardware case

Case 13 is one hardware-gated executable with sections for a consented window and display source when available.

Use `std::binary_semaphore`, `std::condition_variable`, or a bounded polling deadline; do not use nonexistent timed `std::latch` APIs. The case verifies:

- a non-zero frame with an owning texture arrives;
- the concrete item published by the catalog is the item borrowed for capture construction; if it closes after publication but before the final pre-start check, construction fails as typed `SourceUnavailable` without bare-handle re-resolution or retargeting, while a close after that check is accepted as the first published `SourceLost` edge;
- source close produces the typed interruption path when the test source can be controlled;
- `request_stop()` returns promptly, the harness keeps deadline-driven progress until callback quiescence, and no callback target is reached after destruction;
- a small repeated start/stop sample leaves no live capture job, callback, or owned test-frame reference according to explicit RAII counters.

Return 77 when no interactive consented source or desktop is available. Do not claim CI coverage for the skipped path.

## 8. Task 7: Publish the P3 handoff contract

Keep construction inputs concrete and Windows-local: runtime-owned `WindowsCaptureSource`, borrowed D3D device, runtime-owned generation, bounded pool-buffer count, and wake callable. P2 does not include `glfw3native.h`; GLFW-to-`HWND` conversion belongs to P3/composition code after GLFW exists. P3 owns `WindowsRuntimeFactory` and `WindowsReplicaRuntime` because only P3 has both capture and graphics dependencies. Source/job events remain typed local state and are drained/coalesced synchronously by the runtime's next main-thread tick; no application event sink crosses this boundary.

Do not add temporary bridge/runtime interfaces here. Document and compile the P2 headers needed by P3, then verify P2 builds independently before P3 begins.

## 9. Task 8: Capture exclusion and warnings

After a controller/overlay `HWND` exists, the same Windows helper attempts supported display-affinity exclusion and returns portable `CaptureExclusionStatus`: excluded, unsupported, or failed. For the controller, `main` supplies this helper as `DesktopHost`'s window-created callable; the host stores the result before `Application` is constructed. For the overlay, `WindowsReplicaRuntime` emits the result once through its first `RuntimeTickResult`. `Application` projects both into visible warnings and never claims guaranteed exclusion.

This does not receive a separate automated test; behavior is covered by the grouped manual warning scenario because compositor/capture behavior is OS dependent.

## 10. Windows capture completion gate

- [ ] Cases 9, 10, and 12 pass in the policy executable.
- [ ] Case 13 passes or explicitly skips 77 on named Windows hardware.
- [ ] `WindowsCapturedFrame` is move-only and owns the WGC frame checkout, not merely a texture `ComPtr`.
- [ ] Both native delegate trampolines are `noexcept`; projection/allocation/unknown frame exceptions become one fixed fatal category/native code plus edge wake, and callback code contains no Vulkan/GLFW/ImGui/domain calls or blocking wait.
- [ ] Catalog entries install exactly one typed `Closed` observation before publication; the factory and stable source lease share that exact item/state without a second watcher or bare-handle recreation. Each job borrows the item only for session creation, stores no second item member, and owns only its frame pool/session/mailbox.
- [ ] Catalog color rows map active HDR, active SDR, every other successful color-space value, and failed/missing output queries to `HdrDetectedUnsupported`, `Sdr`, `UnknownAssumeSdr`, and `UnknownAssumeSdr` respectively.
- [ ] Initial frame admission remains closed through complete aggregate construction; a matching early arrival uses callback set-then-recheck while opening uses store-then-read without clearing, case 12 forces both interleavings, the final source check precedes `StartCapture`, and successful `StartCapture` remains the last fallible step/publication linearization point.
- [ ] Async delegates hold only weak typed callback state; terminal `request_stop()` contains native revoke/close failures, returns without waiting, retains uncertain cleanup with `stop_quiescent() == false`, and recoverable reconfiguration rejects late/stale epochs, checks source state, performs the same two-sided admission handshake, then services one atomically claimed bounded pending pull without throwing.
- [ ] Hotkey replacement retries cleanup for every uncertain fixed ID, including the primary, before probing; an unverified ID is neither reused nor reported active, and the `noexcept` hotkey WndProc contains command-sink exceptions while emitting only application commands.
- [ ] P3 is the only plan that composes the canonical overlay/capture/graphics runtime owner.
- [ ] No generic native-resource type or exported Win32 API-mocking hierarchy was introduced.
- [ ] No commit or push is performed by this documentation-review task.
