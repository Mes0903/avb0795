# Airelinna V1 Design

**Status:** Architecture revision under iterative review
**Date:** 2026-08-24
**Product name:** Airelinna
**Primary platform:** Windows 11
**Language:** C++23

## 1. Product summary

Airelinna is a low-latency desktop replica utility. The user selects one display or application window in a compact controller and shows it in one always-on-top overlay. The overlay supports Editable and mouse Pass-through modes, bordered and borderless presentation, source-aspect-preserving resize, and configurable 30/60/144/Unlimited render caps.

OnTopReplica is a read-only UX reference. Airelinna does not copy its source, DWM-only architecture, or click forwarding.

## 2. V1 scope

### Goals

- Windows 11 application and display capture through Windows Graphics Capture (WGC).
- One active replica overlay, while domain identifiers and commands remain session-shaped for later expansion.
- Newest-frame rendering with bounded queueing and no frame interpolation.
- Exact GLFW 3.4 windows, Dear ImGui controller, and Vulkan rendering. Both product windows use `GLFW_NO_API`; the pinned GLFW main Win32 class carries one plainly marked source patch removing `CS_OWNDC`, which is incompatible with required `WS_EX_LAYERED` Pass-through.
- A documented V1 supported-hardware floor: every Vulkan device selected for the controller or replica must expose and enable the present-fence feature from `VK_KHR_swapchain_maintenance1` or `VK_EXT_swapchain_maintenance1`. Absence is `UnsupportedGraphics`; CPU-copy fallback replaces only D3D/Vulkan frame interop and does not bypass this WSI safety requirement.
- D3D11-to-Vulkan GPU interop when capability checks pass, with a bounded CPU-copy fallback.
- Editable and Pass-through modes recoverable through both the controller and a configurable global hotkey.
- Bordered and borderless modes, centered aspect fit, and mixed-DPI-aware drag/resize behavior.
- Typed state and errors for source loss, permission denial, hotkey conflict, graphics recovery, and degraded CPU-copy mode.

### Non-goals

- Click forwarding or synthetic pointer events.
- Live source thumbnails.
- Crop, stretch, fill, or region selection.
- Restoring the last source, exact overlay location, or automatic recapture.
- Multiple simultaneous overlays in V1.
- Native HDR output, tone mapping, or formal HDR color fidelity.
- Linux or macOS implementation code in V1.
- A mandatory single-file release.

## 3. Architecture rules

These rules are normative and take priority over illustrative implementation details in the plans.

1. **Frontend stops at application commands.** `ControllerView` renders an immutable `ControllerViewState` and emits `AppCommand` values. It cannot include platform, renderer, GLFW-native, D3D, Vulkan, or Win32 headers and cannot retain a native window handle.
2. **Application owns use-case sequencing.** `Application` is the only component that mutates `ApplicationState`, starts/stops a replica runtime, applies settings, coordinates hotkey/config changes, or arbitrates runtime lifecycle candidates into domain transitions. The runtime translates native facts but does not duplicate portable lifecycle precedence.
3. **Portable domain contains no native resources.** `ReplicaSession`, commands, errors, source descriptors, settings, and capabilities are values. D3D textures, `HWND`, Vulkan handles, synchronization primitives, and type-erased native pointers never enter `core`.
4. **The platform runtime owns the data-plane aggregate.** `WindowsReplicaRuntime` owns the overlay, one stable `WindowsCaptureSource` lease, the current capture job, graphics stack, and their shutdown order. One registry item-observation state canonically owns the exact source item/sole pre-publication `Closed` watcher; the source owns only its runtime lease/wake binding/edge cursor, and `WgcCaptureJob` canonically owns its raw-frame mailbox. The application sees only `IReplicaRuntime` commands/events.
5. **Capture callbacks do bounded handoff only.** An admitted WGC callback may validate metadata, move an owning `WindowsCapturedFrame` checkout into the capture job's newest-only mailbox, record one edge-triggered capture signal, and wake the main loop only on empty-to-ready/signal transition. A current-epoch callback that observes activation/reconfiguration admission closed records one no-throw atomic `arrival_pending` bit, re-reads admission, and wakes if opening won the race; it returns without pulling a frame. Opening stores admitted first, then reads without clearing that bit and wakes if observed. The next runtime tick atomically claims the bit and services the pending pull through the same contained intake path, so either callback/open ordering is covered. Stale-epoch or terminally closed callbacks record nothing. Plain newest-frame replacement does not wake a capped scheduler repeatedly. The callback never calls Vulkan, GLFW, Dear ImGui, an application event sink, `ApplicationState`, or a blocking wait.
6. **Main thread owns UI and presentation.** All GLFW, Dear ImGui, Win32 overlay policy, Vulkan swapchain, bridge/import/upload, render, present, and domain mutation occur on the main thread.
7. **One resource has one canonical owner.** Borrowed references must not outlive that owner. Cleanup order is explicit and tested at subsystem boundaries.
8. **Capabilities describe behavior, not platform names.** Portable UI may branch on a capability that changes visible behavior; it may not branch on `Windows`, `X11`, `Wayland`, or `macOS` strings.
9. **V1 abstractions require a real boundary.** Do not create an interface solely to mock one trivial function. Prefer concrete RAII classes and small pure policies. Keep only the three application ports required by independent responsibilities: source discovery, replica-runtime construction, and global hotkeys.
10. **Tests protect behavior and ownership contracts.** V1 targets roughly 10-20 automated cases, not a test per method or a fake for every class.

## 4. Layers and dependency direction

```text
DesktopHost (event loop and controller-window infrastructure)
    |
    +--> ControllerView (ImGui widgets only)
    | emits AppCommand / reads ControllerViewState
    v
Application (use cases and composition policy)
    | uses portable ports only
    +--> ISourceCatalog
    +--> IReplicaRuntimeFactory --> IReplicaRuntime
    +--> IGlobalHotkey
    +--> ConfigStore
    v
ReplicaSession / ApplicationState (portable domain)

Windows adapters implementing the ports
    +--> WindowsSourceCatalog
    +--> WindowsReplicaRuntime
    |      +--> OverlayWindow + Win32OverlayAdapter
    |      +--> WindowsCaptureSource
    |      +--> WindowsRenderStack
    |      +--> WgcCaptureJob
    |             +--> LatestValueMailbox<WindowsCapturedFrame>
    +--> WindowsGlobalHotkey
```

Allowed dependency direction is downward. Windows adapters can depend on portable values and policies. `core`, `app`, and `ui` cannot depend on `platform/windows`. The executable composition root is the only place that constructs concrete Windows adapters.

## 5. Responsibilities and ownership

| Component | Owns | Must not do |
|---|---|---|
| `ReplicaSession` | Selected source, lifecycle, mode, FPS cap, border preference, status/error values | Call backends, own windows, perform I/O |
| `ApplicationState` | One current `ReplicaSession` and its monotonically replaced `SessionId` generation token | Own a session collection, runtime resources, or rendering |
| `ControllerView` | Dear ImGui widgets and conversion of user gestures into `AppCommand` | Own a window/renderer or call runtime/platform APIs |
| `DesktopHost` | Controller GLFW window, private controller-only Vulkan/ImGui backend including its own present-fence retirement, event loop and UI lifetime | Mutate domain state or call the replica runtime directly |
| `Application` | Command processing, domain transitions, current runtime, loaded config plus its dirty-persistence bit, orchestration | Handle D3D/Vulkan resources or WGC callbacks |
| `ISourceCatalog` | Source enumeration contract | Start capture or create windows |
| `WindowsSourceCatalog` | Native source enumeration plus best-effort per-refresh DXGI `ColorSupport` classification, publishing portable descriptors into the current registry generation | Expose DXGI/output handles, monitor display changes continuously, or make the UI query color state |
| `WindowsSourceRegistry` | Current-generation mapping from opaque `SourceId` to one typed item-observation state created before publication; that state owns the exact `GraphicsCaptureItem`, sole `Closed` token and sticky status | Re-resolve bare native handles, leak native identities upward or retain stale generations |
| `WindowsCaptureSource` | Runtime lease on that same observation, runtime wake attachment/edge consumption and wake-callback quiescence across WGC job generations | Register a second `Closed` watcher, recreate/retarget an item, own a frame pool/render resources or expose a native source upward |
| `IReplicaRuntimeFactory` | Create one platform runtime from source/settings | Mutate application state or retain an application callback |
| `IReplicaRuntime` | Coarse runtime controls: tick, transactional settings, and nonblocking stop request | Expose native handles or platform frame types |
| `IGlobalHotkey` | Validate/register/replace/unregister OS-wide hotkey binding | Change session mode directly; it emits an application command |
| `WindowsReplicaRuntime` | Complete Windows replica lifetime and shutdown order | Render controller UI or mutate domain state |
| `WgcCaptureJob` | One WGC pool/session generation, checked-out-frame mailbox, bounded fatal/resize latches, frame-callback state/token and quiescence | Own source-`Closed` lifetime, bridge/render frames, or own overlay |
| `WindowsRenderStack` | Vulkan instance/device/surface/swapchain, present fences/retired presentation resources, D3D11 device, interop/CPU slots and in-flight retirement for its assigned runtime generation | Enumerate sources, own application state/generation counter, or receive UI commands |
| `OverlayWindow` | Patched `GLFW_NO_API` overlay lifetime and GLFW floating/decorated attributes | Apply raw Win32 policy itself |
| `Win32OverlayAdapter` | Validate the GLFW class has no `CS_OWNDC`/`CS_CLASSDC`, then own `HWND` subclass, transactional input-transparency styles/layered attributes, DPI, hit testing, aspect/border geometry, checked procedure restoration and failure-safe `WM_NCDESTROY` detachment | Attempt layered mode on a forbidden class, mutate GLFW decoration or own Vulkan/capture resources |
| `ConfigStore` | JSON validation and persistence | Know UI or runtime types |

`Application` owns at most one `std::unique_ptr<IReplicaRuntime>` in V1, and `ApplicationState` stores only one current session rather than a speculative collection. `SessionId` changes when that scalar session is replaced, so a queued command projected for the previous session is rejected before mutation. Future multiple overlays may replace the scalar session/runtime pair with a container without changing the frontend-to-Application-to-runtime call direction.

## 6. Portable contracts

Portable values include `SessionId`, `SourceId`, `SourceDescriptor`, `ColorSupport`, `ReplicaSettings`, `PlatformCapabilities`, `CaptureExclusionStatus`, `RuntimeDrainStatus`, `BridgeMode`, `BridgeReason`, `HotkeyChord`, `AppErrorCode`, `AppError`, `ExitDisposition`, `AppCommand`, `RuntimeEvent`, and `ControllerViewState`. P1 owns the complete `AppErrorCode` inventory and its one derived retryability mapping; platform plans translate native failures into those exact codes rather than define local taxonomies. Pre-runtime `PlatformCapabilities` contains only static UI capabilities; GPU bridge mode is reported only after a concrete runtime evaluates its actual D3D/Vulkan resources. `BridgeReason::None` accompanies only GPU interop, capability fallback reports the first failed fact, and `InteropOperationFailure` reports setup/runtime fallback after all facts passed. `ExitDisposition` is Application-owned host state, not another session lifecycle or service.

`ControllerViewState` carries the current `SessionId`; `ControllerView` uses only that projected generation token when constructing session-specific commands and never discovers or retains a session through another object. `Application` rejects a mismatched token as a stale queued command. It also distinguishes the desired typed hotkey from one Application-owned confirmed-binding bit. The bit starts false, becomes true only after successful OS registration, remains unchanged by conflict or cleanup failure, and is cleared by registration loss or shutdown; when true, the confirmed binding is exactly the current in-memory desired chord. The frontend only displays this projected truth and never queries the platform adapter.

The pure controller projection receives all required portable Application-owned facts explicitly: domain state, current session ID, source snapshot, static capabilities/config, controller exclusion, current-runtime bridge/exclusion/drain feedback, runtime-present/stop phase, and application notice. That is enough to distinguish Error with no runtime from Error while a runtime is still draining. It receives no runtime, port, service, or native object.

`HotkeyChord` is typed and validated before registration. It contains modifier flags plus one supported key; display/serialization text is derived from the typed value. An empty string or `string.back()` parser is not part of the contract.

The global hotkey emits only session-agnostic `RestoreActiveReplicaEditable`. Before stop is requested, `Application` resolves it against the current runtime-bearing session; during stop/drain it is a harmless no-op. The Windows hotkey adapter never receives `SessionId`, domain state, overlay, or runtime access. Its hidden-window WndProc is `noexcept`: it catches every exception from the potentially allocating command sink and drops only that restore request so no C++ exception crosses the Win32 callback boundary; the independent controller remains the recovery path.

`RuntimeTickResult` is one portable, allocation-free value containing eight fixed optional event slots plus an optional next wake deadline. Eight is the V1 maximum: three status-only changes plus the five distinct pre-stop lifecycle candidates; after stop, a safe `Stopped` is returned alone. `AppError` contains only its canonical code plus optional fixed-width numeric native detail; user/diagnostic labels are static views, so `Failed { AppError }` and the whole batch are nothrow-copyable and nothrow-movable without dynamic text ownership. Events include one canonical `BridgeModeChanged { mode, reason }`, overlay exclusion, `RuntimeDrainStatusChanged { Normal | Stalled }`, overlay close, source loss, graphics recovery, stopped, and failed. A stalled drain retains ownership; there is no duplicate degraded-to-CPU event. `RuntimeDrainStatus` covers the complete native runtime drain—callback/job cleanup as well as D3/Vulkan/presentation retirement—so no non-graphics teardown failure is mislabeled or silently omitted. `IReplicaRuntime` is intentionally coarse:

```cpp
class IReplicaRuntime {
public:
  virtual ~IReplicaRuntime() = default;
  virtual std::expected<void, AppError> apply(const ReplicaSettings&) noexcept = 0;
  virtual RuntimeTickResult tick(
      std::chrono::steady_clock::time_point now) noexcept = 0; // main thread only
  virtual void request_stop() noexcept = 0; // idempotent; begins nonblocking drain
};
```

`apply` is transactional: an ordinary failure is returned only after the old native settings have been restored and verified. If rollback cannot be verified, it returns terminal `RuntimeStateLost`; `Application` enters Error and requests safe runtime stop instead of claiming the old settings remain effective. Both operations are no-throw port boundaries, and their fixed `AppError` fallback is repeatedly constructible without allocation. Before stop, a `tick` catch discards any partial event batch, returns that failure alone, and preserves a bounded wake while the published runtime still owns resources. During stop, a catch suppresses lifecycle failure, latches drain uncertainty, retains all owners, and returns a bounded observation deadline so repeated exceptions can still reach Stalled instead of sleeping forever. `tick(now)` advances nonblocking callback/graphics work and returns all distinct status changes and pre-stop lifecycle candidates synchronously with the earliest wake deadline. It does not rank portable lifecycle candidates: `Application` alone selects `Failed > OverlayCloseRequested > SourceLost > recovery`, independent of slot order. When one selected recovery outcome contains both causal facts, Application applies `GraphicsRecoveryStarted` then `GraphicsRecovered` in that order rather than discarding completion. Once source loss has made the portable lifecycle `Interrupted`, later recovery-started/completed candidates are harmless self-transitions: native repair may continue for the retained render stack, but only a terminal failure may replace the disconnected lifecycle with Error. After `request_stop()`, the runtime's resource phase suppresses lifecycle candidates except a sole `Stopped` after safe teardown. A `Stopped` event is the only permission for `Application` to release a published runtime.

WGC callbacks never publish directly to `Application`; they wake the loop and leave frame/signal state inside the owning capture job for the next runtime tick. Therefore there is no application runtime-event queue, callback-held application reference, runtime epoch, or stale event from a destroyed runtime. Events carry state/error values only, never native resources. The factory accepts source/settings only and returns either a fully constructed runtime or `AppError`. It first builds the complete local runtime aggregate with frame admission closed, performs its final no-throw sticky source-open check, then invokes `StartCapture` as the last fallible native operation and publication linearization point. A current-epoch `FrameArrived` that observes admission closed sets `arrival_pending`, rechecks admission, and wakes if it became open, without checking out a frame. After `StartCapture` succeeds, no-throw opening first stores admission open and then observes without clearing the bit, waking if set; the first published tick atomically claims and services the pending pull. This two-sided handshake covers both interleavings, so a static source cannot lose its sole notification. Then only `unique_ptr` ownership transfer remains. A source close linearized after the pre-start check is consumed as the first normal `SourceLost` edge. Any earlier failure leaves admission closed, so late delegates cannot check out a frame. Before subclass installation, the factory fully constructs one heap-stable detached adapter/thunk and one initially disarmed concrete no-throw cleanup guard over the overlay/adapter pair. Installing the custom WndProc is the adapter attachment's final fallible action; its success is followed immediately by a no-throw guard arm before any later fallible work, so callback-visible state is complete and there is no unguarded post-subclass interval. The guard follows the partial pair into the local unpublished aggregate and disarms only after successful activation and ownership transfer. It attempts checked detachment first and, if restoration fails, keeps the adapter/thunk alive while explicitly destroying the overlay through forwarded `WM_NCDESTROY`, then destroys the cleared adapter. Other partial objects use ordinary reverse RAII; no successfully started capture is rejected into an unpublished drain path, and no public cleanup interface is introduced.

## 7. Windows runtime data flow

Catalog enumeration filters native window/monitor candidates, immediately creates each candidate's `GraphicsCaptureItem`, and installs the item's sole `Closed` handler before publishing its descriptor. The registry entry stores one concrete typed observation state containing that exact strong COM identity, token, sticky closed bit/edge and callback admission; the delegate weak-captures only that state. Only then does the entry receive a portable `SourceId`. Refresh retires old observation handlers, drops registry references and invalidates their IDs; refresh is disabled while any runtime is retained. Runtime construction resolves a current ID once and gives `WindowsCaptureSource` a lease on the same observation. The source binds a weak runtime wake target, checks the already-sticky state before job construction/start and consumes runtime edges; it neither installs a second handler nor calls `CreateForWindow`/`CreateForMonitor` again. Thus closure between publication and Start is remembered, and a recycled `HWND` or monitor handle cannot retarget selection.

During that same refresh, `WindowsSourceCatalog` privately maps display candidates to their `HMONITOR` output and window candidates to the greatest-overlap output, then reads current `IDXGIOutput6::GetDesc1` color space. Active HDR becomes `ColorSupport::HdrDetectedUnsupported`, active SDR becomes `Sdr`, and a missing output/interface or failed query becomes `UnknownAssumeSdr`. This best-effort snapshot is refreshed only by the existing Refresh command; both non-SDR values project distinct visible warnings through `Application`/`ControllerViewState`. No DXGI handle, polling service, or color query crosses into `Application` or UI, and V1 still performs SDR rendering without tone mapping.

```text
WGC internal callback thread
    -> acquire move-only WindowsCapturedFrame owning Direct3D11CaptureFrame
    -> WgcCaptureJob-owned newest-only mailbox (replace/check in stale frame)
    -> latch bounded fatal/resize signals inside the job
    -> wake main event loop

GraphicsCaptureItem Closed callback (installed before catalog publication)
    -> registry observation's sticky SourceLost edge
    -> weak currently-bound WindowsCaptureSource wake target, if any
    -> wake main event loop

main thread WindowsReplicaRuntime::tick(now)
    -> WgcCaptureJob::service_pending_arrival() once
    -> WgcCaptureJob::drain_signals() once
    -> WgcCaptureJob::take_latest() for the newest raw frame
    -> advance WindowsRenderStack even when no new frame is accepted
       -> GPU-copy into an imported shared slot, or
       -> bounded CPU readback/upload slot
    -> aspect-fit render
    -> present
    -> retire completed slots by Vulkan fence
    -> return RuntimeEvent values plus the earliest scheduler, graphics, or incomplete-drain observation deadline
```

`WindowsCapturedFrame` lives under `platform/windows`, is move-only, and owns the agile `Direct3D11CaptureFrame` checkout itself, including allocation extent, valid `ContentSize`, generation tag and timestamp. Pixel format is immutable job-generation state selected when the frame pool is created/recreated; an observed surface-format mismatch is a fatal capture-contract error, not a resize signal. A texture view is obtained only while that checkout is alive. The wrapper checks the frame back into its pool exactly once through a no-throw close path. A `ComPtr<ID3D11Texture2D>` alone is explicitly insufficient; there is no `void*`, manual detached reference, or `shared_ptr<void>` lifetime token.

### Callback stop contract

Each registry item observation and `WgcCaptureJob` owns one concrete typed callback state by `shared_ptr`; asynchronous delegates capture only a `weak_ptr` to that state and never raw `this`. The observation owns the sole item-`Closed` token, admission and sticky status from before descriptor publication. `WindowsCaptureSource` shares that observation and owns only a weakly referenced runtime wake state plus its edge-consumption cursor. The job state owns frame-registration epoch/admission, one current-epoch atomic `arrival_pending` bit, mailbox, and fatal/resize latches. Both native delegate trampolines are `noexcept`. Item-`Closed`, stale frame epochs, and terminally closed frame callbacks return before touching any latch, mailbox, wake callable, sender, registry, source or job. The sole exception is a matching activation/reconfiguration epoch whose frame admission is temporarily closed: that callback sets `arrival_pending`, re-reads admission and wakes if it is now open, then returns before `TryGetNextFrame`; no frame or native sender is retained.

The item-Closed path uses only no-throw guard/atomic/wake operations. The frame path catches every projection/allocation/unknown exception inside the trampoline, converts it without dynamic text into a fixed capture-failure category plus native HRESULT-style code, sets the existing sticky fatal edge, performs the ordinary no-throw edge wake, and then leaves the guard. No exception crosses the WinRT delegate boundary, and a local frame acquired before a later throw is checked back in by its owning RAII value.

Terminal stop is initiated, not synchronously joined. Job `request_stop()` first performs only no-throw state changes: mark terminal stopping, close frame admission/mailbox, and advance the epoch; it then attempts `FrameArrived` revocation and capture-session close inside complete exception/HRESULT containment and returns while entered callbacks/state remain owned. If either native stop operation is not proven successful, the job records a fixed cleanup diagnostic, retains all potentially live objects, and keeps `stop_quiescent()` false. During source-loss or graphics-recovery cleanup before the runtime's own stop phase, the runtime maps that fact to terminal `Failed { RuntimeStateLost }`; after runtime stop is already requested, lifecycle suppression remains intact, `Stopped` is forbidden, and the same retained fact eventually reports `RuntimeDrainStatus::Stalled` through the bounded drain observation. Source `request_stop()` disables/detaches only its runtime wake target; it does not revoke the registry observation needed by the current source snapshot. A final entered runtime-wake or frame callback exit wakes the main loop. Later ticks poll source callback quiescence and job `stop_quiescent()`; safe frame checkout release gates job destruction, while the self-contained item observation may outlive a source lease and cannot reach it. Registry refresh/destruction, allowed only with no retained runtime, closes observation admission and revokes each sole `Closed` token before dropping entries; an already-entered delegate retains only its typed state. Published-object destructors never wait.

`begin_reconfigure()` likewise closes frame admission, advances the frame epoch, revokes `FrameArrived`, marks pending, and returns. Later ticks wait for frame-callback quiescence, consume the final old mailbox checkout, and complete old-allocation retirement. `prepare_reconfigure(...)` requires an empty mailbox/idle old allocation, then calls frame-pool `Recreate` with the generation's fixed format and installs a new-epoch handler while admission remains closed and its `arrival_pending` bit clear. The runtime performs its final sticky source-open check next; only success calls allocation-free `commit_reconfigure() noexcept`, which stores admission open before reading the bit without clearing it and wakes when set. A callback that observed closed performs the complementary set-then-recheck described above. The next tick services that pending pull before draining capture signals. A closure not observed by the final source check is the next normal `SourceLost` edge. Failure leaves admission closed and terminal cleanup discards the bit, so no second quiescence cycle is hidden. The source watcher remains active throughout reconfiguration and device recovery.

`service_pending_arrival() noexcept`, `take_latest()`, job `drain_signals()`, source `consume_closed_edge()/is_closed()`, and the explicit quiescence/reconfiguration operations are the only P3 consumer surface. The service call verifies the current epoch is admitted, atomically exchanges its pending bit to false, and, only when it claimed true, performs a pool-buffer-count-bounded `TryGetNextFrame` drain through the same exception-contained intake helper as the callback, retaining only the newest checkout; it is a concrete job operation, not a queue or interface. Neither opener clears the bit, which is what makes the two-sided wake handshake race-safe. These operations expose no callback state, mailbox, mutex, or token. A drained batch clears edge notification but still reports sticky fatal state; the runtime merges fixed signals with precedence `FatalCaptureError > SourceLost > Resize`, advances D3 copy completion without a blocking fence loop, and destroys a job only after callback/render leases are safe.

## 8. Graphics ownership and synchronization

`WindowsReplicaRuntime` canonically owns a monotonic resource-generation counter. `WindowsRenderStack` is constructed only after the overlay window exists and receives its immutable initial generation, because the Vulkan surface requires that window. It selects a presentation-capable Vulkan physical device and creates a capture-compatible D3D11 device. A valid matching Windows LUID is required only for the optional shared-texture GPU path; the CPU-copy baseline remains available when LUID discovery or matching fails. Source-allocation rebuilds and device recovery receive the next runtime-owned generation; no reconstructed stack restarts a local counter.

Replica shaders are compiled to SPIR-V and converted to generated C++ byte arrays during the CMake build; `WindowsRenderStack` creates shader modules from those linked spans and never opens a shader file. The statically linked Dear ImGui Vulkan backend likewise introduces no runtime shader-file dependency. This is build/package plumbing, not a shader-loader service or public renderer contract, and the release contains no external `.spv`.

Every selected Vulkan presentation device—both the replica stack and the controller's independent private context—must expose and enable the present-fence feature from `VK_KHR_swapchain_maintenance1` or `VK_EXT_swapchain_maintenance1`. Absence is a typed construction failure rather than an unsafe fallback. Each `vkQueuePresentKHR` attaches one present fence and uses one shared header-only value classifier: success/suboptimal and WSI rejection results specified to enqueue queue operations retain the fence, wait semaphore, and related swapchain resources. Host/device OOM results known not to enqueue release only the unused present fence; the still-acquired image and render-finished semaphore return to submission-drain ownership until its fence signals. The owner then calls maintenance1 release and classifies its result: only `VK_SUCCESS` clears acquired-image state; surface loss retains the record under its swapchain and diverts to ordinary surface teardown; an unknown, validation, or unexpected result is terminal and retains that record until all image uses complete and legal swapchain destruction. A completed unconsumed semaphore is destroyed/replaced rather than reused after its signal operation completes, independent of whether image release succeeded. Device loss enters lost-device teardown; an unknown present result is terminal and conservatively retains possibly referenced resources. Each owner polls retained fences through deadline-aware main-loop progress. Render-submission fences and `vkQueueWaitIdle`/`vkDeviceWaitIdle` are not presentation-retirement proof. The four shared result classifiers are pure value functions plus private owner state, not a presenter service/interface/manager.

Both owners also share one header-only `classify_acquire_result(VkResult)` value function and call `vkAcquireNextImageKHR` with timeout zero—never `UINT64_MAX` or a blocking fence wait. Every attempted acquire supplies an unsignaled binary semaphore plus an unsignaled host-pollable fence. `VK_NOT_READY`/`VK_TIMEOUT` owns no image and returns a scheduler-derived retry deadline; `VK_SUCCESS` and `VK_SUBOPTIMAL_KHR` own the image and both signal operations, with suboptimal requesting recreation only after safe use; out-of-date/full-screen-loss requests swapchain recreation without using an image; replica surface loss enters render-stack-local surface recreation while controller surface loss remains host-fatal; device loss uses the existing lost path; OOM/unrecognized failures are terminal. Vulkan specifies that every unsuccessful acquire leaves the supplied semaphore and fence unaffected, so the owner ignores `pImageIndex` and may recycle those still-unsignaled objects only under their ordinary no-pending-operation preconditions. No result other than success/suboptimal may consume an acquire semaphore or fence as if signaled, and fence status is polled rather than host-waited.

Both owners share a third header-only `classify_submit_result(VkResult)` value function. `VK_SUCCESS` is `Enqueued`; the record retains command state, acquire semaphore/fence, render-finished semaphore, submission fence, acquired image, and source/slot leases until their documented completion points. `VK_ERROR_OUT_OF_HOST_MEMORY`/`VK_ERROR_OUT_OF_DEVICE_MEMORY` are `NotEnqueued`: Vulkan guarantees referenced state is unaffected, so no submission-fence wait is invented; the owner polls the acquire fence, whose signal follows the same acquire's semaphore signal, then invokes maintenance1 for the unused acquired image and routes its result through the shared release classifier. Only release success clears the image record. Surface loss diverts to surface teardown; an uncertain release becomes terminal and keeps the record under the swapchain until all image use is complete and swapchain destruction is legal. The completed acquire semaphore is destroyed/replaced rather than reused, while unaffected command, render-finished semaphore, and unused submission fence state may be recycled or destroyed. `VK_ERROR_DEVICE_LOST` enters lost-device teardown. `VK_ERROR_UNKNOWN` or any other submit result is `Uncertain`: it returns terminal `RuntimeStateLost`, retains every possibly referenced object, and may retire the record only if its submission fence later proves completion; otherwise the existing bounded Stalled-to-process-abandon path prevents unsafe destruction. These acquire/submit/release rows remain sections of case 8, and concrete owners propagate positive polling deadlines without a busy loop.

The fourth shared header-only value function is `classify_release_result(VkResult)`: `VK_SUCCESS` is `Released`, `VK_ERROR_SURFACE_LOST_KHR` is `SurfaceLost`, and `VK_ERROR_UNKNOWN`, `VK_ERROR_VALIDATION_FAILED`, or any unexpected result is `Uncertain`. A non-`Released` result never clears acquired-image state. This classifier adds no resource owner, retry service, or public interface.

Each concrete Vulkan owner has an optional active swapchain and an owned retired set. Before calling `vkCreateSwapchainKHR` with a current handle as `oldSwapchain`, it stops new acquire/submission, progresses ordinary submissions, and invokes maintenance1 for each acquired image that will not be presented only after all image uses complete. Same-surface replacement proceeds only after every required release returns `VK_SUCCESS`; a release-reported surface loss retains the old record and diverts to surface teardown, while an uncertain result is terminal and retains ownership until legal swapchain destruction. The owner then moves the current handle to retired state and leaves active empty before creation. The old handle is retired even if creation fails: success installs the replacement; a non-device failure retains old presentation resources and retries boundedly with `oldSwapchain = VK_NULL_HANDLE`; the retired handle is never reused. Device loss follows the explicit lost-device path.

The GPU path is an optimization after the CPU path works end to end. WGC textures are not assumed to be directly shareable. A small ring of shareable D3D11 textures is created and imported into Vulkan once. For each slot:

1. wait until the slot's previous Vulkan fence is complete;
2. D3D11 `CopySubresourceRegion` of the valid content rectangle from the WGC texture into the matching shareable texture;
3. signal a monotonically increasing shared `ID3D11Fence` target value, with `UINT64_MAX` permanently reserved for the device-removed sentinel;
4. on later main-thread ticks, poll `ID3D11Fence::GetCompletedValue()` without blocking: a value below the target is pending, a non-sentinel value at least equal to the target is complete, and `UINT64_MAX` is `D3DeviceRemoved` rather than completion; do not submit Vulkan work until the complete branch, so the imported timeline wait is already satisfiable;
5. Vulkan waits on the matching completed value of a permanently imported timeline semaphore and acquires the image from the external queue family;
6. Vulkan samples/presents the imported image and releases it back to the external queue family;
7. the Vulkan submission fence marks the slot reusable for the next D3D write.

Each CPU/GPU copy slot retains the owning WGC frame checkout until the D3 fence/query proves the source copy complete, then checks it in. The frame-pool buffer count covers the bounded destination-slot ring plus mailbox/callback handoff without creating an unbounded queue.

Capability checks use `vkGetPhysicalDeviceImageFormatProperties2` with the relevant external-image structures and `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`; require `ID3D11Device5::CreateFence` and immediate-context `ID3D11DeviceContext4::Signal`; query timeline-specific D3D11-fence import through `vkGetPhysicalDeviceExternalSemaphoreProperties` with `VkSemaphoreTypeCreateInfo`; and call `vkGetMemoryWin32HandlePropertiesKHR` for every D3D-created NT texture handle before choosing a compatible Vulkan memory type. Unsupported LUID, interface, handle/type intersection, or semaphore combinations select CPU copy without failing the session.

CPU fallback uses a two- or three-slot recycled pool: D3D11 staging texture, row-pitch-aware copy into mapped Vulkan staging memory, transfer to a device-local image, and Vulkan-fence retirement. It permits at most one unpublished captured frame.

Source-size change is a main-thread barrier, not an in-callback `Recreate`: initiate `begin_reconfigure()`, wait for frame callbacks to quiesce, consume/destroy the final old mailbox checkout, then continue deadline-driven ticks until `render_allocation_idle()` proves both D3 source copies and every Vulkan submission referencing old slots have retired. Only then advance generation, rebuild slots, call `prepare_reconfigure(...)` while admission remains closed, perform the final source-open check, and call no-throw `commit_reconfigure()`. A latched source loss, fatal error, or unexpected surface-format mismatch cancels resize. Frames carry both allocation extent and valid `ContentSize`; only the valid subrectangle is copied/sampled.

### Graphics recovery

Every capture/graphics allocation belongs to a monotonically increasing runtime-owned resource generation, while one `WindowsCaptureSource` remains immutable across generations. Replica `VK_ERROR_SURFACE_LOST_KHR` is presentation-local: `WindowsRenderStack` stops new acquire/present work, retires ordinary Vulkan submissions and present-fence records, recreates only `VkSurfaceKHR` plus swapchain-dependent objects against the same overlay, and preserves the WGC job, D3 device, bridge/CPU slots, and resource generation. Capture may continue replacing the one newest mailbox frame while presentation is unavailable. The stack reports surface-recovery started/completed facts to the runtime; it does not ask the runtime to stop capture. Both facts may be returned in one runtime batch when recovery starts and completes during one tick, and Application treats that pair as one causal recovery outcome.

For `VulkanDeviceLost` or confirmed `D3DeviceRemoved`, the runtime:

1. initiates nonblocking stop of the current WGC job but keeps the stable source watcher active;
2. clears/rejects new mailbox work and retains the job/frame pool while callbacks or render slots still hold checkouts;
3. retains independent Vulkan-device-lost and D3-device-removed facts; a later failure promotes only the affected cleanup rule;
4. for Vulkan device loss, uses Vulkan's lost-device rule for child cleanup without waiting failed Vulkan fences, while still retaining D3 resources/checkouts until D3 completion is proven;
5. only for confirmed D3 device removal, cancels slots whose D3 fence value was not observed complete and which therefore have no Vulkan submission, then releases those checkouts without a D3 completion signal; a slot submitted only after observed D3 completion has no canceled external wait and its healthy Vulkan work retires normally unless Vulkan device loss is independently confirmed;
6. after callback/source-copy safety and the cause-specific graphics conditions are established, destroys the old WGC job/frame pool and device-dependent graphics stack while keeping source plus overlay/adapter alive;
7. recreates graphics and a new WGC job from the same stable source under the next generation, then resumes only with new-generation frames.

A bounded retry policy decides retry versus terminal failure; `WindowsReplicaRuntime` maps exhausted graphics recovery, terminal capture failure, terminal Vulkan OOM/unknown result, or unrecoverable active-device failure to `Failed { AppErrorCode::RuntimeStateLost }`. The policy is pure and testable, while Vulkan ownership and native diagnostic values remain concrete. V1 uses three construction attempts per swapchain/surface/device-recovery episode with delays of 0 ms, 100 ms, and 500 ms. An episode resets only after one successful present from the replacement; merely constructing it does not reset the budget. The concrete coordinator owns each counter: `WindowsRenderStack` for replica swapchain/surface recreation, `WindowsReplicaRuntime` for replica device rebuild, and `DesktopHost` for controller recreation. Runtime drain polls unresolved native ownership no later than every 50 ms and reports Stalled after 10 seconds with no forward cleanup progress; `WindowsReplicaRuntime` alone owns that aggregate timer/status, while `DesktopHost` owns the equivalent controller-drain timer and host-fatal result. Forward progress means a tracked callback/job/adapter/D3/Vulkan/presentation ownership fact becomes proven safe or is retired; another poll, retry, warning edge, or newly added pending fact does not reset the timer. While work remains, the next deadline is the earlier of `now + 50 ms` and the no-progress threshold. Progress clears a published Stalled status to Normal and starts a fresh no-progress interval. These are concrete constants beside the existing policy, not configurable settings or another service.

## 9. Window and input behavior

- The overlay is always on top while active.
- Bordered mode uses OS chrome.
- Borderless Editable mode uses `WM_NCHITTEST` for caption drag and DPI-scaled edge/corner resize.
- `WM_SIZING` preserves source aspect during native resize; render-time aspect fit remains the safety net.
- `WM_DPICHANGED` applies the suggested rectangle and updates physical-pixel hit-test bands.
- GLFW 3.4's upstream main Win32 class includes `CS_OWNDC`, while Microsoft forbids `WS_EX_LAYERED` on `CS_OWNDC`/`CS_CLASSDC`. Airelinna's Vulkan-only static build applies one checked-in, license-marked patch removing only `CS_OWNDC` from that main class; the helper class is unchanged and OpenGL contexts are unsupported. CMake verifies the exact pinned source/patch, the controller's existing Windows setup callable checks the first actual class before Application construction, and `Win32OverlayAdapter` repeats the guard before subclassing.
- On Windows 11, `Win32OverlayAdapter` is the single owner of Pass-through state for both border modes. It snapshots the full prior extended style and any restorable layered attributes, adds `WS_EX_LAYERED | WS_EX_TRANSPARENT`, then calls `SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)` and verifies the opaque layered result. Exit restores the exact snapshot; any failed stage rolls back and verifies, with unverifiable rollback becoming `RuntimeStateLost`. An un-restorable pre-existing layered state is rejected before mutation, and Airelinna does not use `UpdateLayeredWindow`. This avoids relying on GLFW's cross-platform contract, which guarantees mouse pass-through only for undecorated windows. `HTTRANSPARENT` remains supplementary because its documented forwarding scope is same-thread windows. Manual verification must confirm both modes deliver input to another process beneath the overlay.
- The custom overlay WndProc is `noexcept` and contains every C++ exception, falling back to the saved GLFW procedure without mutating adapter state when an internal operation cannot complete. Detachment is explicit and checked with the Win32 zero-return/last-error convention. Normally the adapter restores the original GLFW procedure and is destroyed before the window. If restoration fails, the runtime keeps adapter/thunk state alive while destroying the GLFW window; the custom procedure forwards `WM_NCDESTROY` to the saved procedure, clears its binding, and only then may the adapter be destroyed.
- `OverlayWindow` alone toggles `GLFW_DECORATED`. The runtime coordinates an adapter snapshot/restore of the client rectangle around that call; the adapter never rewrites the decorated style bits. Border transitions preserve the client/content rectangle as closely as monitor work-area clamping permits.

## 10. State, errors, and shutdown

The lifecycle transition table is normative; omitted command/state pairs are rejected or harmless no-ops as specified by `Application`:

| From | Trigger and guard | To | Runtime disposition |
|---|---|---|---|
| Idle or Error with no runtime | accepted Start with a selected source | Starting | no runtime yet |
| Starting | factory succeeds and publishes the complete runtime | Capturing | retain the new runtime |
| Starting | factory/permission/source construction fails | Error | no runtime |
| Capturing or RecoveringGraphics | selected SourceLost outcome | Interrupted | retain the disconnected runtime/overlay |
| Capturing | selected recovery-started outcome | RecoveringGraphics | retain the runtime |
| RecoveringGraphics | selected recovery-completed outcome | Capturing | retain the recovered runtime |
| Interrupted | selected recovery-started and/or recovery-completed outcome | Interrupted | allow internal repair of retained graphics without hiding source disconnection |
| Capturing, Interrupted, or RecoveringGraphics | accepted Stop/Quit or selected overlay-close outcome | Stopping | set `stop_requested`, request stop, and retain the runtime |
| Interrupted | accepted replacement Start | Stopping | retain the old runtime and one pending Start until `Stopped` |
| Capturing, Interrupted, or RecoveringGraphics | terminal apply/runtime failure | Error | set `stop_requested`, request stop, and retain the draining runtime |
| Stopping | safe `Stopped` after an ordinary Stop | Idle | release the empty runtime |
| Stopping | safe `Stopped` with a pending replacement | Starting, then Capturing or Error from the factory result | release old runtime before constructing the replacement |
| Error with a retained draining runtime | safe `Stopped` | Error with no runtime | release only the empty runtime; Start/Refresh then become available |

Quit uses the same retained-runtime stop path but completes through `ExitDisposition::Clean` rather than requiring another visible lifecycle. A Stalled quit/fatal drain retains the runtime and selects `AbandonProcess`. An unexpected `Stopped` without `stop_requested` is a protocol failure and preserves Error after releasing the reported-empty runtime.

`ApplicationState` transition methods return command results to `Application`; it does not provide a misleading `last_error(SessionId)` backed by one global error slot. `Application` owns one portable optional application notice, a private `stop_requested` bit for its current runtime, and the host-facing `ExitDisposition`; no frontend or platform adapter may set those values. Runtime events are processed on the main thread: status-only changes are applied first, then Application selects one winning lifecycle outcome while that bit is false. Recovery started/completed in the same selected outcome are applied causally in that order; lower-priority outcomes are still ignored. Once `stop_requested` is true, Application accepts only `Stopped` regardless of whether visible lifecycle is Stopping or Error. On `Stopped`, it releases the empty runtime, clears the bit, and completes quit, starts a pending replacement, preserves terminal Error, or transitions a normal stop to Idle. Runtime-scoped bridge, overlay-exclusion, and drain feedback resets on runtime publication/release; recovery remains owned by `ReplicaSession`, and controller-window exclusion remains process-scoped.

`Application::step` and `shutdown` are no-throw host boundaries. A violated port contract or any other unexpected exception while native owners may be live enters the existing fail-closed abandon disposition, retains those owners, and returns control to `DesktopHost`; it never unwinds a published runtime. `DesktopHost::run` likewise contains an unexpected controller/UI exception, stops new controller work, drives only the no-throw application shutdown path as far as possible, and returns `abandon_process = true` so `main` uses the existing non-unwinding exit. These are exception firewalls around existing owners, not a new error service or recovery hierarchy.

The controller uses a small private Vulkan context owned by `DesktopHost`; it is not shared with the replica runtime in V1. This keeps controller rendering and low-latency replica/device recovery lifetimes independent. It is a concrete implementation detail, not another public renderer interface. `DesktopHost` uses timeout-zero result-classified acquire, propagates positive pending deadlines, and checks its own acquire/submit/present results. Controller surface/device loss or another terminal graphics result is host-fatal in V1, so it stops controller submissions, asks `Application` to shut down through its existing public host operation, and continues headless application ticks until Application selects `Clean` or `AbandonProcess`; the host then reports diagnostics and returns the corresponding non-zero process result. Surface loss still requires ordinary controller submission/presentation retirement before surface-owned destruction; only confirmed Vulkan device loss uses lost-device child cleanup without waiting failed Vulkan fences. Unknown terminal results fail closed rather than treating possibly referenced resources as abandoned.

Normal shutdown order is:

```text
stop accepting UI commands
-> unregister hotkey
-> if config remains dirty from an earlier command-time save failure, retain an unsaved-settings diagnostic without retrying synchronous file I/O
-> runtime.request_stop(): detach source runtime wake, close/revoke job callback admission/handler and return
-> close the capture-job frame mailbox
-> continue deadline-driven runtime ticks until callback quiescence plus D3/Vulkan submission/presentation completion; never block
-> release slot-held WGC checkouts only after D3 completion or confirmed D3 removal
-> destroy capture job and frame pool
-> destroy render stack and Vulkan surface after ordinary present-fence retirement
-> detach Win32 subclass and destroy adapter before overlay; on restoration failure destroy overlay while adapter/thunk remains alive through forwarded WM_NCDESTROY, then destroy adapter
-> quiesce/release the stable capture-source wake state and observation lease
-> runtime emits RuntimeEvent::Stopped
-> Application releases the now-empty runtime
-> Application sets ExitDisposition::Clean and keeps any unsaved-settings diagnostic visible while DesktopHost is still available
-> stop controller submissions and retire its own pending present fences/resources
-> DesktopHost::run returns; destroy Application
-> retire registry item observations and destroy Windows services
-> destroy DesktopHost controller graphics/window/ImGui
-> terminate GLFW
```

`RuntimeDrainStatus::Stalled` is not release permission during normal operation. After hotkey clear, dirty-config diagnostic retention, and runtime stop initiation, `Application` evaluates both the retained current drain status and later status edges: an already-Stalled runtime selects `ExitDisposition::AbandonProcess` immediately without waiting for a duplicate edge, while a runtime that later crosses to Stalled selects the same disposition then. In either path it retains the runtime and never fabricates `Stopped`. `DesktopHost` stops new controller work and returns an abandon result without destroying native owners; `main` flushes the diagnostic sink and calls `std::_Exit` with a non-zero code before stack unwinding. This one exceptional composition-root path prevents an infinite shutdown while avoiding unsafe native destruction; it is not a runtime manager, recovery service, or normal Error path.

The process establishes and verifies effective `PER_MONITOR_AWARE_V2` through its manifest or an early helper before creating either GLFW window. The main loop is event-driven (`glfwWaitEventsTimeout` or equivalent deadline-aware wait), not an unlimited `glfwPollEvents` busy loop. A command emitted while rendering the controller sets a host-local flag and skips the following wait so the command cannot sleep in the main-thread deque. If runtime-event handling requests stop, publishes a replacement after `Stopped`, or `shutdown()` is called between steps, `Application` schedules its own immediate next-step deadline; this is combined with the runtime deadline so post-tick work cannot sleep forever when the prior runtime returned no deadline. While any callback, job-stop, D3/Vulkan, presentation, adapter, or source-wake drain remains incomplete, the runtime deadline also includes a strictly future bounded observation/stall-threshold poll even when no external callback or graphics completion is expected to wake the loop.

## 11. Persistence, privacy, and packaging

Config v1 stores only version, typed hotkey, FPS cap, and border preference. Editable/Pass-through mode is session-only and never triggers persistence. Config stores no source, title, pixels, or window position. `Application` alone owns one config-dirty bit: every accepted hotkey/FPS/border mutation sets it before persistence, and only a successful command-time save clears it. A failed save leaves the accepted in-memory/runtime value and dirty bit intact; the next accepted config-backed command retries persistence while the application is running. Shutdown retains an unsaved-settings diagnostic but does not call synchronous persistence before native drain. A temporary file is flushed and atomically replaced through one platform-supplied replace callable; a virtual filesystem hierarchy is not introduced for this one operation.

Capture begins only after explicit source selection and Start. Pixels and source titles are not transmitted. Logs omit source titles and pixel contents by default. Self-capture exclusion is best effort: the controller result is obtained before `Application` construction, the overlay result returns as a nonfatal runtime event, and `ControllerViewState` visibly warns when either is unsupported or failed.

GLFW and Dear ImGui are built from pinned source dependencies. A single EXE is preferred, but Windows/Vulkan/system runtime DLLs are allowed and documented.

## 12. Verification budget

V1 uses **15 named automated cases** as a budget, not a requirement to split every assertion into a new test.

### Portable deterministic cases (8)

1. Application lifecycle through real ports: start/asynchronous stop/source loss/reselection/overlay close/ordinary-versus-terminal apply-result handling, same-batch recovery start/completion plus recovery self-transitions while Interrupted, Error-preserving `Stopped` disposition, sole portable runtime-event arbitration/status reset, immediate-step scheduling, config boundary including shutdown no-retry, plus stale-`SessionId` command rejection.
2. Newest-only mailbox replacement, close, and move-only destruction.
3. FPS scheduler for 30/60/144/Unlimited and missed-deadline skip.
4. Config defaults, validation, round trip, atomic-replace failure cleanup, and allowed keys.
5. Typed hotkey validation and serialization.
6. Aspect-fit and MAILBOX-to-FIFO policy.
7. Controller view-state projection, including capability-disabled controls, application notice, and warnings.
8. Seven bounded graphics-policy groups: result-classifier tables without cross-products; representative submit/present ownership traces; retired-swapchain and surface-only recovery preserving capture/D3/generation; source-size prepare/check/commit and old-allocation idleness; D3 completed-value/removal ownership; Vulkan/D3 device recovery plus bridge selection; and exact retry/no-progress timing boundaries with repeated-exception whole-runtime drain deadlines/Stalled retention. Portable lifecycle arbitration remains only in case 1.

### Windows policy/contract cases (4)

9. Source eligibility/filtering, current-process windows, registry generation rejection, and SDR/HDR/unknown color classification without a native-item test double.
10. Hotkey replacement rollback, desired-versus-confirmed Application projection including loss followed by cleanup failure, session-agnostic restore-command emission, and Win32 callback exception containment.
11. GLFW main-class compatibility, Win32 overlay input-transparency including opaque layered attributes/exact restoration, no-throw WndProc behavior, factory-local ordered cleanup after a post-subclass construction failure, hit-test, DPI, aspect-resize, border geometry, and concrete settings-transaction rollback policy.
12. Closed-admission publication/reconfiguration with both sides of the current-epoch pending-arrival/open handshake, shared item-observation/runtime-wake and WGC late/no-throw callback safety, no-throw stop-cleanup failure retention, nonblocking quiescence and prepare/check/commit reconfiguration, source/job edge semantics, immutable pool-format validation, and move-only frame ownership using controlled adapters; P3-only merge/slot/device-loss behavior remains in case 8.

### Hardware-gated integration cases (3)

13. WGC window/display capture using the catalog-published concrete item, closed-before-start rejection without retargeting, and clean stop.
14. CPU-copy capture-to-present path with nonblocking acquire, resize/resume, present-fence-aware bounded stop observation, and resource-stability sampling.
15. GPU interop when supported plus real capability-query wiring and selected-path verification; deterministic each-fact fallback belongs to case 8, and no synthetic device-loss hook is required.

Manual release verification covers seven grouped scenarios: source selection/loss/permission, overlay resize/DPI/border, patched-class/cross-process Pass-through recovery, FPS caps, hotkey conflict, capture exclusion/HDR warning plus validation-clean controller present retirement, and package/runtime dependency/altered-source audit.

Do not add a test solely for a trivial struct, compiler-language setting, capability boolean, fake implementation, or duplicate application scenario. New tests require a distinct failure mode not already represented by these cases.

## 13. Post-V1

- Multiple active runtimes/overlays.
- HDR metadata, tone mapping, and native HDR output.
- Wayland Portal/PipeWire and X11 backends.
- macOS ScreenCaptureKit backend.
- Optional crop/region features.

The post-V1 list creates no V1 implementation classes or unused extension points.

## 14. Documentation and Git policy

These spec and plan files remain uncommitted working-tree material until the user explicitly authorizes a commit. The plans do not authorize commit, push, remote creation, or history rewriting. Test implementation is likewise not committed by this documentation-review task.
