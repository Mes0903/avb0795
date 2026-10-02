# Airelinna V1 Overlay and Renderer Implementation Plan

> **For agentic workers:** Run after P2 has defined `WindowsCapturedFrame` and WGC ownership. This document does not authorize a commit or push.

**Goal:** Implement the Windows overlay, native interaction policy, Vulkan/D3D graphics stack, CPU-copy baseline, optional GPU interop, bounded recovery, and final Windows runtime composition.

**Architecture:** Native window and graphics resources stay in concrete Windows RAII classes. The renderer consumes a move-only Windows frame on the main thread; no generic bridge or virtual Vulkan driver crosses into portable layers.

**Tech Stack:** C++23, GLFW, Vulkan 1.3, Win32, D3D11/DXGI, Vulkan Win32 external memory/semaphore extensions.

---

## 1. Files and responsibilities

| Path | Responsibility |
|---|---|
| `platform/windows/overlay_window.*` | GLFW overlay lifetime |
| `platform/windows/win32_overlay_adapter.*` | WndProc subclass, DPI, hit testing, chrome and Pass-through |
| `third_party/patches/glfw-3.4-win32-no-own-dc.patch` | Marked one-line patch removing `CS_OWNDC` from GLFW's main window class for this Vulkan-only build |
| `render/render_math.*` | Pure aspect-fit and present-mode policy |
| `render/graphics_retry_policy.*` | Pure bounded retry/device-generation policy and shared Vulkan acquire/submit/present-result classification |
| `render/bridge_policy.*` | Pure CPU/GPU bridge selection from queried capability facts |
| `render/windows_render_stack.*` | Vulkan/D3D ownership, bridge slots, render/present/recovery |
| `shaders/replica.vert`, `shaders/replica.frag` | Build-time Vulkan shader sources; never opened at runtime |
| `cmake/EmbedSpirv.cmake`, build-tree `generated/airelinna/render/embedded_shaders.*` | Convert generated SPIR-V into C++ byte arrays linked into the graphics target |
| `platform/windows/windows_replica_runtime.*` | Ordered aggregate owner of stable capture source, overlay, graphics and current WGC job |
| `platform/windows/windows_runtime_factory.*` | `IReplicaRuntimeFactory` implementation |
| `tests/render/render_policy_test.cpp` | Named cases 6 and 8 |
| `tests/windows/overlay_policy_test.cpp` | Named case 11 |
| `tests/windows/windows_graphics_hardware_test.cpp` | Named cases 14 and 15 |

Do not create `IVulkanDriver`, `IFrameBridge`, `IWindowsInteropApi`, generic render-frame leases, or a UI-visible native window API.

## 2. Task 1: Add dependencies and targets when first used

- [ ] Pin exact GLFW 3.4 source and apply the checked-in, plainly marked one-line patch that changes only its **main** Win32 window-class registration from `CS_HREDRAW | CS_VREDRAW | CS_OWNDC` to `CS_HREDRAW | CS_VREDRAW`. Microsoft forbids `WS_EX_LAYERED` on `CS_OWNDC`/`CS_CLASSDC`; both Airelinna GLFW windows are Vulkan-only `GLFW_NO_API`, so no OpenGL/DC context is supported by this patched build. Keep the helper-window class unchanged. CMake configuration reads the pinned source after patching and fails if the exact main-class line still contains `CS_OWNDC` or if the expected pinned line changed, preventing a silent version drift. Record the altered-source patch and GLFW license in third-party notices.
- [ ] Find Vulkan 1.3 plus CMake's `glslc` component when the first overlay/render source is created, and invoke the imported `Vulkan::glslc` executable rather than adding another shader-tool dependency. Enforce the documented V1 supported-hardware floor: each selected presentation device must expose and enable the present-fence feature from `VK_KHR_swapchain_maintenance1` or `VK_EXT_swapchain_maintenance1`; absence returns `AppErrorCode::UnsupportedGraphics`, not an unsafe or separately engineered fallback. Windows Pass-through is owned by the Win32 adapter because GLFW's public contract guarantees that attribute only for undecorated windows.
- [ ] Build GLFW statically. Dear ImGui is deferred to P4 because this plan does not render the controller.
- [ ] Under `if(WIN32)`, create `airelinna_windows_graphics` only after `render_math.cpp` and the first Windows source exist.
- [ ] Link `airelinna_core`, `airelinna_app`, `airelinna_windows`, GLFW, Vulkan, D3D11, DXGI, and user32 with the narrowest visibility.
- [ ] Compile the two shader sources to SPIR-V through explicit CMake custom commands, then run the one small `EmbedSpirv.cmake` step to generate C++ byte arrays. Compile that generated translation unit into `airelinna_windows_graphics` and create shader modules from `std::span<const std::uint32_t>` over the embedded data. Declare the complete source -> SPIR-V -> embedded C++ dependency chain; no runtime `.spv` path, loader, copied shader file, or extra packaging artifact exists.
- [ ] Add cases 6, 8, and 11 to existing `catch_discover_tests(... ADD_TAGS_AS_LABELS)` executables. Register one hardware executable containing cases 14 and 15 only when Windows integration is enabled, using `catch_discover_tests(... ADD_TAGS_AS_LABELS SKIP_IS_FAILURE PROPERTIES SKIP_RETURN_CODE 77)` so the helper does not append its default skip code 4. Its custom main bypasses environment preflight only for Catch2 list/discovery arguments, performs no graphics/capture work then, and otherwise returns 77 before entering the requested case when interactive-desktop/source or mandatory present-fence-feature preflight is unavailable.

Expected: target generation succeeds at every task boundary; include roots use `${PROJECT_SOURCE_DIR}/include`.

## 3. Task 2: Implement pure render and recovery policies

### Aspect fit and present mode

`aspect_fit(source, destination)` returns no viewport for a zero extent. Otherwise it computes the largest centered integer viewport preserving the source ratio. Rendering always clears the full framebuffer to opaque neutral black before drawing the viewport.

Present-mode selection prefers MAILBOX when advertised and otherwise chooses FIFO. It never chooses IMMEDIATE as an automatic Unlimited fallback.

### Test case 6

One table-driven case covers 16:9 into square, portrait into landscape, equal aspect, odd dimensions/centering, zero extent, MAILBOX available, and FIFO fallback.

### Graphics retry policy

Create one concrete pure `GraphicsRetryPolicy`; do not wrap the Vulkan driver in a virtual interface. It receives result category, current attempt, current generation, and elapsed no-progress time, then returns a retry/surface-recreate/device-rebuild/terminal decision plus any candidate next generation and next deadline; it never stores or owns counters/timestamps. Swapchain, surface, and device reconstruction each use one recovery episode with exactly three construction attempts scheduled at 0 ms, 100 ms, and 500 ms. Exhaustion returns terminal failure. The concrete coordinator owns and resets its episode only after the replacement produces one successful present: `WindowsRenderStack` owns replica swapchain/surface counters, `WindowsReplicaRuntime` owns replica device-rebuild attempts plus the monotonic resource generation, and `DesktopHost` owns controller recreation attempts. Construction success alone cannot hide an immediate repeated failure. `WindowsReplicaRuntime` separately owns one whole-runtime drain no-progress timestamp/status; `DesktopHost` owns the equivalent timer only for its private controller drain and maps expiry to its host-fatal result. While native ownership remains unresolved each owner polls no later than every 50 ms, reaches its 10-second no-progress boundary, and uses the earlier of `now + 50 ms` or that threshold as its deadline. Retiring or proving safe a tracked callback/job/adapter/D3/Vulkan/presentation fact is progress; polling, retrying, emitting a warning, or adding another pending fact is not. Runtime progress clears its published Stalled status to Normal and restarts the interval. Presentation-pending or uncertain cleanup always forbids complete, and expiry never authorizes unsafe release. Case 8 checks these fixed boundaries as values, while case 14 exercises the actual Vulkan fence query—no Vulkan driver mock or timing service is added.

Keep four small header-only free functions beside this policy and share them with the replica and controller Vulkan owners. `classify_acquire_result(VkResult)` distinguishes acquired (`VK_SUCCESS`, `VK_SUBOPTIMAL_KHR`), pending (`VK_NOT_READY`, `VK_TIMEOUT`), recreate (`VK_ERROR_OUT_OF_DATE_KHR` and enabled full-screen loss), surface/device loss, and terminal OOM/unrecognized errors. Every unsuccessful acquire owns no image, ignores the undefined output index, and leaves its supplied semaphore/fence unaffected; those still-unsignaled objects may be recycled only under their ordinary no-pending-operation preconditions. `classify_present_result(VkResult)` treats `VK_SUCCESS`, `VK_SUBOPTIMAL_KHR`, and the WSI rejection results that Vulkan specifies as still enqueueing queue operations (`VK_ERROR_OUT_OF_DATE_KHR`, `VK_ERROR_SURFACE_LOST_KHR`, `VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT`, and `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT` when enabled) as `Enqueued`; the attached present fence and wait semaphore remain tracked. Present `VK_ERROR_OUT_OF_HOST_MEMORY`/`VK_ERROR_OUT_OF_DEVICE_MEMORY` are `NotEnqueued`: only the unused present fence is released immediately, while the still-acquired image plus render-finished semaphore return to submission-drain ownership. After the render submission fence signals, invoke maintenance1 for that unused image and classify the result; only release success clears its acquired-image record. The completed semaphore is destroyed/replaced—never reused—after its signal operation completes. Device loss enters the existing lost path, and an unrecognized present result is terminal and conservatively retains possibly referenced resources unless its fence later signals or explicit device-loss teardown makes destruction legal.

`classify_release_result(VkResult)` maps only `VK_SUCCESS` to `Released`, `VK_ERROR_SURFACE_LOST_KHR` to `SurfaceLost`, and `VK_ERROR_UNKNOWN`, `VK_ERROR_VALIDATION_FAILED`, or any unexpected result to `Uncertain`. `SurfaceLost` retains the acquired record under its swapchain and diverts to ordinary surface teardown. `Uncertain` is terminal and likewise retains the record until all image uses complete and swapchain destruction is legal. A failed release never clears acquired-image ownership.

`classify_submit_result(VkResult)` classifies `VK_SUCCESS` as `Enqueued`, host/device OOM as specification-guaranteed `NotEnqueued`, device loss as `DeviceLost`, and unknown/other failures as `Uncertain`. Every successful/suboptimal acquire supplies both a binary semaphore and a host-pollable acquire fence. For `NotEnqueued`, the owner waits only by polling that acquire fence; once it signals after the acquire semaphore signal, it calls maintenance1 for the unused image and classifies that result. Only `Released` clears the image; `SurfaceLost` diverts to surface teardown and `Uncertain` becomes terminal, with either failed outcome retaining the record under the swapchain until legal destruction. The completed acquire semaphore is destroyed/replaced, while unaffected command state, unsignaled render-finished semaphore, and unused submission fence may be reset/recycled or destroyed without pretending that fence will signal. An `Uncertain` submit retains the complete record and can release it only after a later submission-fence proof or lost-device teardown; otherwise it reaches the existing Stalled/process-abandon fail-safe. These are value classifiers and concrete records, not presenter classes, ports, or managers.

### Test case 8

Case 8 is deliberately capped at seven flat named `SECTION` groups; table loops inside a group have no nested `SECTION`s or Cartesian products:

1. one acquire/submit/present/release classifier table with one row per documented category and the unsuccessful-acquire unchanged-sync invariant;
2. three representative ownership traces in one group: submit OOM after acquire-fence proof including release success/surface-loss/uncertain rows, uncertain submit retained to proof/Stalled, and present ownership through submission completion plus the same release outcomes;
3. retired-swapchain handling for one successful and one failed replacement, plus one surface-loss recovery trace proving that WGC/D3/slots/generation remain live and that started/completed facts can share one result;
4. source-size reconfiguration: old-allocation idleness, stale-generation rejection, prepare with admission closed, final source check, commit, and one representative source/job signal merge;
5. D3 completed-value pending/complete/`UINT64_MAX` rows plus one pre-submit cancellation and one post-completion Vulkan-retirement trace;
6. device recovery and bridge selection: representative Vulkan-loss and D3-removal cleanup, all-true/first-failed capability rows, and one safely retired `InteropOperationFailure` fallback;
7. whole-runtime drain progress: the three 0/100/500 ms recovery attempts and successful-present reset, 50 ms poll and 10-second no-progress boundary/reset, pending/complete/Stalled, job-stop uncertainty, two consecutive exceptional ticks spanning stop initiation, strictly future observation deadlines, retry exhaustion, and retained ownership. Use one short scripted timeline rather than a timing cross-product.

Portable lifecycle precedence is tested only by Application case 1. Do not add another top-level test or expand these groups into a fault cross-product.

### Bridge selection policy

`BridgeCapabilityFacts` is a concrete value populated only by `WindowsRenderStack` after it performs the real LUID, D3D interface, Vulkan external-image/semaphore, import-procedure, dedicated-allocation, and per-handle memory-type queries. One pure free function `select_bridge(const BridgeCapabilityFacts&)` returns `GpuInterop` plus `BridgeReason::None` only when every required fact is true; otherwise it returns `CpuCopy` plus the first failed `BridgeReason` in the P1 declaration order. It owns no handles and is not a driver interface.

Case 8 group 6 uses an all-true row, one row per first-failed fact, one pre-submit setup failure, and one safely retired runtime interop failure mapping to `InteropOperationFailure`. Rows are independent inputs to the pure selector, not nested combinations. This verifies deterministic fallback/reason behavior on one machine without fabricating Vulkan/D3D objects or adding a top-level case.

## 4. Task 3: Implement overlay and Win32 policy

### `OverlayWindow`

`OverlayWindow` owns exactly one `GLFWwindow*`. It sets `GLFW_CLIENT_API` to `GLFW_NO_API`, creates an always-on-top Vulkan overlay from the patched GLFW class, owns GLFW floating/decorated attributes plus the GLFW close-request flag, and provides one idempotent no-throw `destroy()` used by the ordered runtime teardown; its destructor calls that same operation if still needed. It exposes the native GLFW pointer only to Windows implementation files; no `ui` or `app` public header includes it.

The overlay object does not call raw Win32 APIs. Normally the adapter detaches and is destroyed first. If checked WndProc restoration fails, the current coordinator keeps the adapter/thunk state alive while `OverlayWindow::destroy()` destroys the `HWND`; only after the adapter observes `WM_NCDESTROY` and clears its binding may the adapter be destroyed. The coordinator is `WindowsReplicaRuntime` after publication and one concrete factory-local cleanup guard before publication. That guard is created while the adapter is still detached and is armed without throwing immediately after attachment succeeds; neither coordinator transfers window ownership to the adapter.

### `Win32OverlayAdapter`

The factory first creates the adapter/thunk as a complete heap-stable object in a detached state. It receives the overlay window, obtains `HWND`, calls `GetClassLongPtrW(hwnd, GCL_STYLE)`, and returns `AppErrorCode::UnsupportedWindowClass` before attachment if `CS_OWNDC` or `CS_CLASSDC` is present. All callback-visible state and the disarmed outer cleanup guard exist before attachment. Saving/installing the GLFW WndProc is the final fallible attachment action; success is followed only by a no-throw guard arm before later construction continues, after which the adapter canonically owns restoration. The custom procedure is `noexcept`, catches every C++ exception, and forwards to the saved GLFW procedure without committing partial adapter state when an internal operation cannot complete. It handles:

- mode change: snapshot the full prior extended style and, if already layered, its restorable `GetLayeredWindowAttributes` values; enter Pass-through by adding `WS_EX_LAYERED | WS_EX_TRANSPARENT`, then call `SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)` so the layered overlay remains fully opaque; reject an un-restorable pre-existing layered state before mutation;
- `WM_NCHITTEST`: Pass-through may additionally return `HTTRANSPARENT`; borderless Editable returns DPI-scaled edges/corners and central `HTCAPTION`; bordered mode delegates normal chrome behavior;
- `WM_SIZING`: constrain the actively dragged edge/corner to source aspect without changing the opposite anchor;
- `WM_DPICHANGED`: apply the suggested outer rectangle and recompute physical hit-test bands;
- border geometry: snapshot the current client rectangle before `OverlayWindow` changes `GLFW_DECORATED`, then derive/apply the target outer rectangle without rewriting GLFW-owned decorated style bits;
- no click forwarding;
- explicit checked WndProc detachment before normal overlay destruction, with `SetLastError(0)`/`GetLastError()` disambiguation for a zero `SetWindowLongPtrW` return;
- failure-safe teardown that forwards `WM_NCDESTROY` to the saved GLFW procedure, clears the native binding, and keeps all thunk state alive until `glfwDestroyWindow` returns.

`WindowsReplicaRuntime` calls the adapter for mode changes and coordinates adapter client-rectangle snapshot/restore around `OverlayWindow::set_border`. This makes the adapter the only owner of Win32 input-transparency bits and `OverlayWindow` the only owner of GLFW decoration. The mode transaction verifies both the resulting style bits and opaque layered attributes. Leaving Pass-through restores the exact prior style and saved layered attributes, rather than blindly clearing `WS_EX_LAYERED`; if any enter/restore stage fails, the adapter rolls back and verifies the snapshot before returning `AppErrorCode::PlatformOperationFailure`, or `AppErrorCode::RuntimeStateLost` when verification fails. Airelinna never uses `UpdateLayeredWindow`. `HTTRANSPARENT` is supplementary only because its documented forwarding scope is same-thread windows. No synthesized input is emitted.

The adapter stores current mode, border preference, and source aspect. The source aspect comes from the selected descriptor initially and updates from validated capture metadata.

### Test case 11

Extract only geometry/hit-test/style decisions into pure functions. The same production case also exercises the one concrete internal settings-transaction algorithm through a tiny recording operation object with controlled failures; this is a compile-time test seam, not a virtual native-API interface or general mock framework. One table/section-driven Windows policy case covers:

- Editable center/edges/corners versus Pass-through;
- GLFW class-style acceptance with neither DC bit, and rejection of `CS_OWNDC`/`CS_CLASSDC` through the same small production validator;
- Editable removal versus Pass-through addition of the exact two input-transparency style bits;
- ordered opaque-layer setup (`SetLayeredWindowAttributes` with alpha 255), exact restoration of a prior layered/non-layered snapshot, and controlled failure at the alpha stage;
- DPI scaling of the resize band;
- horizontal and vertical `WM_SIZING` anchors;
- bordered/borderless content-to-outer conversion;
- ordinary failure after each staged mode/border/geometry operation restores and verifies all prior native state;
- rollback verification failure returns terminal `AppErrorCode::RuntimeStateLost`;
- controlled WndProc-restoration failure selects destroy-with-adapter-alive, forwards/clears on `WM_NCDESTROY`, and never calls dead adapter state;
- successful attachment followed immediately by a simulated later construction failure is handled by the already-armed factory-local guard before any runtime exists, including the restoration-failure branch;
- an injected internal exception is contained by the production `noexcept` WndProc and falls back to the saved GLFW procedure without partial state mutation.

The checked-in patch is also verified by the CMake exact-source guard and the adapter's actual `GetClassLongPtrW` startup check. Actual bordered/borderless cross-process click-through and mixed-monitor behavior remain one grouped manual scenario, not dozens of simulated message tests.

## 5. Task 4: Build the CPU-copy vertical slice first

`WindowsRenderStack` is concrete and non-copyable. Its constructor requires an existing `OverlayWindow&`; creation order is therefore enforced by the type signature.

It canonically owns:

- Vulkan instance, physical device, logical device, surface and queues;
- swapchain, image views, framebuffers or dynamic-rendering equivalents, descriptors, pipeline, command pools/buffers and synchronization;
- one present fence per present call classified as enqueued or conservatively unknown, plus any wait semaphore/retired swapchain that has not reached presentation retirement;
- capture-compatible D3D11 device and immediate context; adapter matching is required only by the optional GPU path;
- two or three CPU fallback slots;
- generation assigned by the owning runtime and in-flight retirement state.

Expose only narrow Windows-runtime operations:

```cpp
enum class DeviceLossKind { VulkanDeviceLost, D3DeviceRemoved };

class WindowsRenderStack final {
public:
  WindowsRenderStack(OverlayWindow&, std::uint64_t resource_generation);
  ID3D11Device& capture_device() noexcept;
  bool ready_for_frame() const noexcept;
  void submit(WindowsCapturedFrame&&); // precondition: ready_for_frame()
  RenderTickResult tick(core::Extent framebuffer,
                        std::chrono::steady_clock::time_point now);
  bool render_allocation_idle() const noexcept;
  std::expected<void, core::AppError> rebuild_capture_slots(
      std::uint64_t generation, core::Extent allocation, DXGI_FORMAT format);
  void resize(core::Extent framebuffer);
  void begin_drain() noexcept;
  void begin_device_recovery_drain(DeviceLossKind) noexcept;
  std::uint64_t generation() const noexcept;
};
```

`DeviceLossKind { VulkanDeviceLost, D3DeviceRemoved }` and `RenderTickResult` are Windows implementation values; neither enters application headers. The result contains presentation-local surface-recovery edges, device-recovery requirement, nonblocking D3/Vulkan submission-and-presentation drain progress, and the earliest deadline needed to poll pending work. The stack may retain independent `vulkan_device_lost` and `d3_device_removed` facts while draining so a later loss promotes cleanup without collapsing unrelated causes. `render_allocation_idle()` is true only when the render stack has no old-allocation D3 source copy/WGC checkout, Vulkan command/submission, imported/shared image, or CPU staging/image slot in flight; it deliberately says nothing about the job mailbox or unrelated current-swapchain presentation.

`tick` calls `vkAcquireNextImageKHR` only for a non-retired active swapchain and always uses timeout `0`; each attempt supplies an unsignaled binary semaphore and unsignaled acquire fence, and neither Vulkan owner may use `UINT64_MAX` or wait synchronously for that fence. Success/suboptimal owns the returned image plus both signal operations; only that branch may submit a command waiting on the semaphore. Suboptimal completes safe use and marks recreation pending. `VK_NOT_READY`/`VK_TIMEOUT` submits and presents nothing and returns a strictly future, bounded/backed-off retry deadline from the existing graphics retry policy. Out-of-date/enabled full-screen loss enters swapchain recreation; surface loss enters the stack's presentation-only surface-recovery state; `VK_ERROR_DEVICE_LOST` returns `VulkanDeviceLost`; OOM/unrecognized errors are terminal and the runtime maps them to `AppErrorCode::RuntimeStateLost`. Every unsuccessful branch ignores `pImageIndex`; because no image was acquired and Vulkan leaves both supplied sync objects unaffected, they remain unsignaled and may be recycled under the ordinary no-pending-operation preconditions. The pending path never immediately loops.

Every queue submission uses the shared submit classifier and one concrete submission record. Success retains every referenced object until the submission fence proves completion. For specification-guaranteed not-enqueued OOM, the unused submission fence is never polled as if pending: retain the acquired image/acquire semaphore until the acquire fence signals, then invoke maintenance1 and classify its result. `Released` alone clears the image record; `SurfaceLost` diverts to surface teardown and `Uncertain` is terminal, with both failed outcomes retaining the record under the swapchain until legal destruction. The completed acquire semaphore is destroyed/replaced, while unaffected command state, unsignaled render-finished semaphore, and unused fence may be reset/recycled or destroyed. Device loss follows lost-device teardown. Unknown/other submit failure emits terminal `RuntimeStateLost`, marks the record uncertain, retains all possibly referenced acquire/command/semaphore/fence/slot state, and polls only real proof; failure to obtain proof crosses the existing bounded Stalled threshold and requires P4's non-unwinding abandon path.

`begin_drain()` stops accepting submissions for normal stop. Surface recovery remains inside the stack: stop new acquire/present work, retire ordinary Vulkan submission/presentation records, destroy/recreate only `VkSurfaceKHR` and swapchain-dependent objects, then resume with the same D3 device, slots, and generation. `begin_device_recovery_drain(kind)` is reserved for `VulkanDeviceLost` and `D3DeviceRemoved`: Vulkan loss may destroy lost-device children without waiting failed Vulkan fences but still waits for D3 copy completion; confirmed D3 removal may cancel only a pre-submit D3 copy whose completion value was never observed, while every slot submitted after observed D3 completion must retire healthy Vulkan work normally. A later independent loss promotes only the affected cleanup fact. Later `tick` calls make this progress without `vkDeviceWaitIdle`, an unbounded wait, or a busy loop. Passing the observation threshold reports diagnostic-stalled while retaining every resource that is not proven safe; when progress resumes, the result returns to normal. `WindowsReplicaRuntime`, not this replaceable stack, owns the monotonic generation counter and last published drain status.

Every `vkQueuePresentKHR` call chains one `VkSwapchainPresentFenceInfoKHR` or EXT alias. The stack applies the shared present classifier: every result known to have enqueued queue operations retains the fence, present wait semaphore, and any associated retired swapchain until the fence signals. A known not-enqueued OOM releases only the unused present fence; the acquired image and render-finished semaphore remain owned by the preceding submission record until its fence signals. It then calls maintenance1 and applies the shared release classifier: only `Released` clears the acquired record, `SurfaceLost` diverts to ordinary surface teardown, and `Uncertain` is terminal; failed outcomes retain the record under the swapchain until all image uses complete and legal destruction. The completed render-finished semaphore is destroyed/replaced rather than reused. Device loss follows the lost-device path; an unknown present result fails closed and retains possibly referenced resources. Submission fences, `vkQueueWaitIdle`, and `vkDeviceWaitIdle` are not treated as proof that the presentation engine released those resources. The tracker is concrete internal stack state, not a presenter interface/manager. Ordinary stop and out-of-date recreation cannot complete while presentation retirement or release ownership is unresolved. After `VK_ERROR_DEVICE_LOST`, Vulkan's lost-device rule ends in-use status: destroy all child objects in dependency order without waiting failed fences and never reuse them.

### CPU slot algorithm

1. select a slot whose prior Vulkan fence and D3D completion value are done;
2. move the owning WGC checkout into the slot, copy only its valid `ContentSize` subrectangle to the correctly sized D3D11 staging texture, and signal a per-slot fence/query;
3. on a later tick, poll completion without blocking; map with the supported nonblocking path and honor `RowPitch` for every row;
4. copy to persistently mapped Vulkan staging memory sized with overflow checks; use host-coherent memory or flush a `nonCoherentAtomSize`-aligned written range before submission;
5. record image transition/upload/draw/present commands;
6. associate the slot with the Vulkan submission fence;
7. release the slot's captured-frame lease only after the D3D completion value proves the copy no longer uses it.

If all slots are busy or readback is not ready, keep bounded latency by leaving only the newest raw frame in the runtime mailbox, dropping stale work, and reporting a counter; do not allocate another slot or block the main thread indefinitely. `RenderTickResult` supplies a short bounded poll deadline while CPU work is pending and no deadline once only an external frame/resize event can make progress.

Handle zero framebuffer extent by suspending presentation until a non-zero resize event. Coalesce swapchain resize work on the main thread.

### Source allocation barrier

When the job reports changed `ContentSize`, `WindowsReplicaRuntime` enters a main-thread `ReconfiguringSource` path: call nonblocking `WgcCaptureJob::begin_reconfigure()` and destroy the triggering local checkout. After `frame_callbacks_quiescent()` becomes true, consume/destroy any final old-epoch mailbox checkout exactly once. Continue ticking until `render_allocation_idle()` proves every render-held D3/Vulkan old-allocation reference retired. Only then increment generation, rebuild slots, call `prepare_reconfigure(...)` with frame admission still closed, perform the stable source's final open-state check, and call no-throw `commit_reconfigure()`. A higher-priority fatal/source-lost latch or unexpected surface-format mismatch cancels resize. Diagnostic-stalled keeps every unsafe resource owned while the loop remains responsive; device loss enters the typed recovery path. No old frame, checkout, imported/staging resource, or Vulkan submission crosses the barrier. Case 8 group 4 has one representative old-allocation/commit trace; cases 14 and 15 retain resize coverage without another top-level test.

### Hardware case 14

One hardware-gated case runs a consented WGC source through forced CPU copy, verifies real timeout-zero acquire makes deadline-driven progress without blocking, resizes the source and verifies capture/present resumes, resizes/minimizes/restores the overlay without freezing the event loop, verifies non-black content where practical, samples frame/drop counters, requests runtime stop, and checks that `Stopped` waits for real present-fence retirement plus stable slot/resource counts within the harness deadline. The executable-level preflight returns 77 before Catch2 starts when no interactive desktop/source is available.

A separate mailbox microbenchmark or universal latency assertion is not added.

## 6. Task 5: Add optional GPU interop internally

GPU interop is an internal `WindowsRenderStack` path selected only after the CPU vertical slice works.

### Capability gate

Collect these facts and pass them once to `select_bridge`; require all of:

- Vulkan physical-device LUID is valid and matches the D3D adapter;
- `vkGetPhysicalDeviceImageFormatProperties2` reports importable D3D11 texture external-memory support for the selected BGRA format/usage;
- the capture device queries to `ID3D11Device5`, the immediate context queries to `ID3D11DeviceContext4`, and `ID3D11Device5::CreateFence` plus immediate-context `ID3D11DeviceContext4::Signal` are available;
- D3D11 shared fences and Vulkan timeline semaphores are supported;
- `vkGetPhysicalDeviceExternalSemaphoreProperties` is called with `VkSemaphoreTypeCreateInfo{VK_SEMAPHORE_TYPE_TIMELINE}` chained to `VkPhysicalDeviceExternalSemaphoreInfo` for `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT`, and reports an importable compatible handle type;
- required Win32 import procedures are present;
- every created NT handle reports at least one Vulkan memory type compatible with the matching image requirements.

Any failed fact selects CPU copy and emits the decision's one `BridgeModeChanged { CpuCopy, reason }`; it is not a fatal session error and there is no duplicate degraded event. The concrete stack performs queries and resource construction; the pure selector only decides from their results.

### Shared slot ring

Do not assume the WGC texture is shareable. Create a fixed ring of D3D11 textures with shareable NT handles and import each once into matching Vulkan memory/image/view objects. For every D3D-created NT handle, call `vkGetMemoryWin32HandlePropertiesKHR`, intersect its `memoryTypeBits` with the corresponding Vulkan image memory requirements, and fall back to CPU if the intersection is empty. Honor `DEDICATED_ONLY` external-memory requirements when reported. Use `ID3D11Device5::CreateFence` to create a shared `ID3D11Fence`, verify the timeline-specific external-semaphore properties above, import its handle permanently into a timeline semaphore created with `VkSemaphoreTypeCreateInfo`, and submit monotonically increasing values signaled through the immediate `ID3D11DeviceContext4`; the target allocator never emits `UINT64_MAX`, which is reserved as the device-removed sentinel.

Per frame:

1. choose a slot whose previous Vulkan submission fence completed;
2. move the owning WGC checkout into the slot and issue the valid-subrectangle D3D11 copy to a matching shared texture;
3. signal the next non-`UINT64_MAX` D3D11 fence target after the copy;
4. on later ticks, classify `ID3D11Fence::GetCompletedValue()` without blocking: lower than the target is pending, a non-`UINT64_MAX` value at least equal to the target is complete, and `UINT64_MAX` is `D3DeviceRemoved`; submit nothing outside the complete branch;
5. submit Vulkan work waiting on that now-satisfiable imported fence value and acquire the image from `VK_QUEUE_FAMILY_EXTERNAL` with the required layout/access transition;
6. sample, render, present, then release the image back to `VK_QUEUE_FAMILY_EXTERNAL`;
7. mark the slot reusable for the next D3D write only after its Vulkan submission fence completes.

The slot releases its captured-frame lease after the D3D fence proves the copy finished; it does not rely on a detached raw pointer or implicit command-buffer lifetime.

NT handles have explicit RAII wrappers and close exactly once after import/ownership transfer rules are satisfied. Images, memory, views, fences, semaphores, and D3D resources are destroyed in dependency order by the stack.

An import/setup failure after all capability facts passed falls back immediately and emits `BridgeModeChanged { CpuCopy, InteropOperationFailure }`. A non-device-lost interop failure after submission stops assigning new interop slots, retires existing slots only when their fences complete, then switches permanently to CPU copy for that runtime generation and emits the same reason once. A confirmed D3 removal may cancel only a slot still waiting for an unobserved D3 completion value; because Vulkan submission is deferred until that value is observed, no healthy Vulkan queue can be left waiting on a canceled D3 signal. Device loss uses the explicit abandon path below. No possibly in-flight slot is silently discarded or reused. No CPU/GPU slot is consumed on an acquire-pending/error result because no swapchain image or signaled acquire synchronization object exists on that branch.

## 7. Task 6: Implement graphics recovery

Each concrete Vulkan owner represents presentation state as an optional active swapchain plus an owned retired set. On out-of-date, it stops new acquire/submission, progresses ordinary submission fences, and invokes the enabled maintenance1 release operation for any acquired image that will not be presented only after all image uses complete. Same-surface `vkCreateSwapchainKHR` proceeds only after every required release classifies as `Released`; `SurfaceLost` keeps the record under the old swapchain and diverts to surface teardown, while `Uncertain` is terminal and retains ownership until legal swapchain destruction. Immediately before a permitted replacement call with the current handle as `oldSwapchain`, the owner moves that handle to the retired set and leaves the active slot empty, because the call retires `oldSwapchain` even when replacement creation fails. Success installs the new active swapchain. A non-device failure leaves no active swapchain, retains the retired swapchain and its present records until their fences signal, and makes a bounded retry with `oldSwapchain = VK_NULL_HANDLE`; a retired handle is never reused as `oldSwapchain`. Device loss uses the existing lost-device path. A render-submission fence is not a swapchain-retirement barrier. Replica surface loss follows the same ordinary retirement proof, then recreates the Vulkan surface and swapchain-dependent resources inside this stack without touching capture/D3 ownership.

`WindowsReplicaRuntime` owns `next_resource_generation`; the render stack only carries its assigned generation. The runtime also owns one immutable `WindowsCaptureSource` resolved once by the factory and retained across all job generations. A replica surface loss is completed entirely by the current `WindowsRenderStack`; the runtime may translate its started/completed facts into portable graphics-recovery candidates, including both facts in one result when recovery completes within one tick, but it does not stop or recreate the WGC job and does not advance resource generation. Application alone applies such a selected pair in causal order.

On `DeviceRecoveryRequired { DeviceLossKind }`:

1. call the current job's nonblocking `request_stop()` and `begin_device_recovery_drain(kind)`, retaining job/frame pool/source;
2. keep ticking until frame callbacks are quiescent and D3 source-copy safety is established;
3. for `VulkanDeviceLost`, never infer D3 cancellation: release WGC checkouts only after D3 fence/query proof, while Vulkan lost-device children are destroyed without waiting failed Vulkan fences;
4. for confirmed `D3DeviceRemoved`, use the explicit canceled-copy path, but continue ordinary Vulkan submission/presentation retirement unless a later Vulkan call independently confirms device loss;
5. retain independent loss facts during the drain, and destroy the old job/frame pool/device-dependent stack only after the applicable proof above while stable source plus overlay/adapter remain;
6. increment generation, construct a new stack for the same overlay, create a new job from the same source item with the source-open commit check, then emit a graphics-recovered candidate or allocation-free `Failed { AppErrorCode::RuntimeStateLost }` when the bounded retry budget is exhausted.

No old-generation frame, slot, descriptor, or callback target survives into the new stack.

### Hardware case 15

One hardware-gated case:

- uses GPU interop when the capability gate passes;
- otherwise records the unsupported interop facts with Catch2 `INFO`/`SUCCEED` and verifies automatic CPU fallback without invoking Catch2 `SKIP`; case 14 already exercises the explicit CPU path on capable hardware.

The same case records real query wiring/results for valid/matching LUID, `ID3D11Device5`/`ID3D11DeviceContext4`, timeline D3D11-fence import, and per-handle memory-type compatibility; it verifies the actual selected path against `select_bridge`. Independent false-fact branches are deterministic rows in case 8 rather than claims about hardware case 15.

Do not add a synthetic device-loss hook, five child-process scenarios, or claims of deterministic device-loss injection on unsupported drivers. Generation and retry behavior remains in deterministic case 8; an actual recovery is recorded manually only when the environment provides a supported reproducible trigger.

## 8. Task 7: Compose the Windows runtime and factory

Move final runtime composition here because it depends on both P2 capture types and this plan's overlay/graphics types.

`WindowsRuntimeFactory` implements `IReplicaRuntimeFactory`. Its composition-root constructor borrows `WindowsSourceRegistry`, stores the main-loop wake callable, and fixes the bounded destination-slot/pool-buffer policy; `create` receives only portable source/settings. It resolves the selected current-generation entry once, gives one stable `WindowsCaptureSource` a lease on that entry's already-created item/sole pre-publication observation, and binds the runtime wake state without recreating the item, looking up a native handle, or registering another `Closed` watcher. It then constructs:

```text
WindowsCaptureSource
-> OverlayWindow
-> fully construct a heap-stable detached Win32OverlayAdapter/thunk
-> create a disarmed concrete factory-local no-throw cleanup guard over that pair
-> attach/subclass as the final fallible adapter action, then arm the guard without throwing
-> WindowsRenderStack
-> inactive WgcCaptureJob using stable source plus render stack D3D device
-> otherwise complete WindowsReplicaRuntime aggregate
-> move the overlay/adapter into the local aggregate while the cleanup guard remains armed
-> call WgcCaptureJob::start_for_publication(): final no-throw source-open check, then StartCapture as the last fallible operation/publication linearization point
-> no-throw frame-admission open and unique_ptr publication; then disarm the local guard
```

Immediately after the overlay `HWND` exists, the factory/runtime applies best-effort capture exclusion and stores its portable result as one initial nonfatal runtime event. After actual CPU/GPU slots are selected, the initial result also contains exactly one `BridgeModeChanged` event with its concrete reason. Failure does not expose `HWND`, fail capture construction, or call `Application` directly.

Before publication, frame admission remains closed while the factory translates a forbidden GLFW class to `AppErrorCode::UnsupportedWindowClass`, missing mandatory Vulkan/present-fence support to `AppErrorCode::UnsupportedGraphics`, and other overlay/graphics/job construction failures to `AppErrorCode::RuntimeConstructionFailure`. It fully constructs a heap-stable detached adapter/thunk and a disarmed guard before attachment. WndProc attachment is the adapter's final fallible action and success is followed immediately by a no-throw guard arm; from that point through local aggregate construction and activation, the guard remains armed. On failure it attempts checked detachment and, if restoration fails, keeps adapter/thunk state alive, calls idempotent `OverlayWindow::destroy()`, lets forwarded `WM_NCDESTROY` clear the binding, and only then destroys the adapter. Other inactive/not-successfully-started partial resources unwind in ordinary reverse order. After the aggregate exists, private activation performs the final no-throw source-open check and then `StartCapture` as the final fallible operation/publication linearization point. Success is followed only by no-throw admission opening, ownership transfer, and guard disarm; a close racing after the check is the first published `SourceLost` edge, never a reason to synchronously destroy a successfully started unpublished session. During a published runtime, a native settings operation that rolls back and verifies returns `AppErrorCode::PlatformOperationFailure`; only an unverified rollback returns `AppErrorCode::RuntimeStateLost`. Native HRESULT/`VkResult` details remain diagnostic data and do not form a second error taxonomy.

`WindowsReplicaRuntime` owns those objects, the monotonic resource-generation counter, scheduler, current settings, last published runtime-status values, and the main-loop wake callable. The registry observation owns the sole source-`Closed` token/sticky state; `WindowsCaptureSource` owns its runtime lease/wake binding and edge cursor; each `WgcCaptureJob` owns only frame-generation state/mailbox/fatal-resize latches. No callback targets an application sink. Use optional/`unique_ptr` members so stop and recovery reset resources in dependency order.

Each no-throw `tick(now)` calls source `consume_closed_edge()`, job `service_pending_arrival()`, and job `drain_signals()` exactly once in that order, then calls `take_latest()` only when scheduler/render capacity permit a frame or when a quiescent reconfiguration must discard its final old frame. Pending-arrival service is a pool-buffer-count-bounded pull through the job's existing contained intake path, never a wait or second event queue. The tick always advances pending/render-drain slots even without a new frame, observes the overlay close flag, and returns the earliest FPS, graphics, or incomplete-drain observation deadline; a final callback exit separately wakes the loop. Whenever source wake, callback, job-stop, adapter, D3, Vulkan, or presentation cleanup remains incomplete, the deadline is a strictly future bounded poll no later than the stall threshold even if no external wake is expected. The runtime aggregates those facts into its one whole-runtime drain status. It emits `RuntimeDrainStatusChanged { Stalled }` once when any retained native teardown fact crosses the bounded observation threshold and `{ Normal }` once when all such progress resumes; neither edge releases resources. This aggregation is private runtime state, not another manager or interface.

After merging capture, overlay, and render facts, the runtime puts every distinct status change and pre-stop lifecycle candidate into the fixed eight-slot result without ranking portable outcomes. `Application` is the sole owner of `Failed > OverlayCloseRequested > SourceLost > recovery` and applies a selected same-batch recovery start/completion pair causally. The runtime still owns its native stop phase: after `request_stop()`, it suppresses non-`Stopped` lifecycle candidates and returns a lone `Stopped` only after safe teardown. Keep native signal coalescing as a small concrete runtime operation, not a dispatcher, event bus, or virtual policy hierarchy. Case 8 exercises one representative source/job merge; case 1 owns all portable lifecycle arbitration.

`apply() noexcept` stages native operations one command at a time. Mode changes go only through `Win32OverlayAdapter`; border changes snapshot geometry through the adapter, call `OverlayWindow::set_border`, then restore geometry; scheduler state commits last. `AppErrorCode::PlatformOperationFailure` is returned only after the prior native state is restored and verified. If rollback cannot be verified or an internal exception escapes a staged native operation, return terminal `AppErrorCode::RuntimeStateLost`; `Application` enters Error and calls `request_stop()` rather than publishing stale logical settings. Keep this cross-owner sequencing in one private `WindowsReplicaRuntime` transaction algorithm parameterized at compile time by a small operation object, so case 11 can inject deterministic failures without exporting a platform interface or allowing another owner to apply settings.

`request_stop()` is idempotent and `noexcept`. Its bounded first phase disables the source runtime wake binding, closes job callback admission/mailbox, invokes the job's exception-contained handler/session stop attempt, starts graphics drain, and returns without waiting. A native job-stop failure keeps `stop_quiescent()` false and every job owner retained. If it occurs while stopping a job for source loss or graphics recovery before the runtime stop phase, the next arbitration emits terminal `Failed { AppErrorCode::RuntimeStateLost }`; after runtime stop has begun, it cannot produce `Stopped`, and the bounded whole-runtime drain observation eventually reports Stalled instead. It does not revoke the registry observation's sole `Closed` handler. Later `tick(now)` calls perform quiescence and retirement:

```text
source runtime-wake detach, job stop initiation and job-mailbox close
-> begin graphics drain
-> deadline-driven ticks observe callback quiescence and poll known D3/Vulkan submission/presentation completions
-> release slot-held WGC checkouts only after D3 completion/removal proof
-> WGC job/frame-pool destruction
-> render stack/surface destruction only after ordinary present-fence retirement, or explicit lost-device child cleanup
-> checked Win32 adapter detachment; on success destroy adapter, then overlay
-> on detachment failure keep adapter/thunk alive, destroy overlay, clear binding on forwarded WM_NCDESTROY, then destroy adapter
-> quiesce/release the stable source wake state and observation lease; the registry observation remains until snapshot retirement
-> emit RuntimeEvent::Stopped
```

No tick blocks on callback quiescence, `vkDeviceWaitIdle`, or an unbounded fence wait. Every incomplete drain returns a bounded observation deadline; a diagnostic-stalled classification is therefore reachable even when a failed native stop produces no future callback or graphics wake. All unsafe resources stay owned while polling continues, and the runtime publishes Stalled/Normal transition edges. `Application` never destroys the published runtime before `Stopped`. During normal operation, Stalled grants no release permission. If P4 selects `ExitDisposition::AbandonProcess` after a stalled quit/fatal drain, the runtime remains owned and no native destructor runs; main terminates the process without fabricating `Stopped`. At a normal `Stopped`, the runtime is an empty shell, so its destructor is nonblocking; partial factory construction uses weak callback state/local RAII before publication. Runtime callbacks target weak typed source/job state, never raw `this`, a container element, or an application object. `tick() noexcept` catches every internal exception and clears any partial fixed event batch. Before stop it returns only the repeatedly constructible allocation-free `Failed { RuntimeStateLost }` plus a bounded wake while resources remain live. During stop it emits no `Failed`, latches drain uncertainty, retains all owners, and returns a bounded observation deadline on every exceptional tick until Stalled is reported or safe teardown is proven.

Source loss initiates nonblocking job stop, lets callback/slot-held checkouts retire through later ticks, then destroys the job/frame pool while retaining the stable closed source, overlay, and render stack with a disconnected presentation. Any observed GLFW overlay close flag produces `OverlayCloseRequested`, independent of Editable/Pass-through or bordered/borderless state; the Editable bordered close button is only the primary visible gesture. Only `Application` translates that event into its normal Stop transition. A replacement Start creates a new runtime only after the interrupted runtime emits `Stopped`.

## 9. Overlay/renderer completion gate

- [ ] Cases 6, 8, and 11 pass; case 11 exercises the production internal settings transaction with controlled failures rather than only Application-facing fake results.
- [ ] Cases 14 and 15 pass, or the whole executable honestly skips with return 77 only when executable-level environment preflight fails; unsupported GPU interop inside case 15 records facts and verifies CPU fallback without Catch2 `SKIP`.
- [ ] Render stack construction requires an existing overlay window.
- [ ] Shader build dependencies generate SPIR-V and embed it into `airelinna_windows_graphics`; the runtime opens no shader file and the package contains no external `.spv`.
- [ ] CPU copy is a working bounded baseline before GPU interop.
- [ ] CPU copy remains available without a valid/matching Vulkan LUID; GPU interop additionally requires `ID3D11Device5`, immediate `ID3D11DeviceContext4`, timeline-fence import, and compatible per-handle memory types. Capability failures report their exact first failed reason, while setup/runtime fallback after passing capabilities reports `InteropOperationFailure`.
- [ ] GPU slots use explicit external memory, classify D3D11 completion as pending/complete/device-removed without mistaking `UINT64_MAX` for completion, defer Vulkan submit until D3 completion, and use Vulkan-fence retirement.
- [ ] Both Vulkan owners acquire only with timeout zero using a binary semaphore plus acquire fence, treat pending results as a future deadline without submit/present/busy-loop, consume acquire synchronization only for success/suboptimal, and classify recreate/loss/terminal results explicitly.
- [ ] Both Vulkan owners use the shared submit classifier: success retains a normal submission record, OOM waits for acquire-fence proof before attempting maintenance release without waiting an unused submission fence, device loss uses lost-device teardown, and uncertain submit failure retains all possibly referenced objects through proof or process abandonment.
- [ ] Each presentation device supports KHR/EXT swapchain-maintenance present fences; both owners classify every maintenance image release, clear acquired ownership only on `VK_SUCCESS`, divert surface loss, and retain uncertain results under the swapchain through legal destruction. Completed unconsumed semaphores are destroyed/replaced without reuse.
- [ ] Same-surface recreation starts only after required image releases succeed; failed `vkCreateSwapchainKHR` with `oldSwapchain` leaves the active slot empty, never reuses the now-retired handle, and retries boundedly with `VK_NULL_HANDLE` while retirement remains owned.
- [ ] Pending CPU-copy slots progress without a new captured frame and publish a bounded wake deadline without busy polling.
- [ ] Pre-publication frame admission stays closed until the complete aggregate and final pre-start source check; successful `StartCapture` is the last fallible operation/linearization point, and only no-throw opening/ownership transfer follows, so no successful start can require an unpublished tick loop.
- [ ] The adapter/thunk and disarmed concrete local guard exist before attachment; the attach operation exposes success only after a no-throw arm. A later pre-publication failure uses that guard, checked restoration failure destroys the overlay while adapter/thunk state remains alive through forwarded `WM_NCDESTROY`, and case 11 covers this combination without a new top-level test.
- [ ] Stop initiation and source/job reconfiguration never wait for callbacks; a matching closed-admission arrival records one pending bit, prepare installs the new epoch with admission closed, the final source check precedes no-throw commit/wake, the next tick services the bounded pull, typed state remains until quiescence, and only a lone `Stopped` permits runtime destruction.
- [ ] Runtime consumes one source edge plus one job signal batch, conditionally consumes `take_latest()`, emits all distinct pre-stop lifecycle candidates without ranking them, and leaves portable outcome arbitration plus same-batch recovery-pair ordering solely to Application.
- [ ] Stalled never fabricates `Stopped`; case 8 verifies retained ownership and bounded progress across repeated exceptional stop ticks, while P4 case 1 owns the process-abandon decision during shutdown.
- [ ] WGC checkouts remain alive through D3 copy completion and are returned before their frame pool is destroyed.
- [ ] Source allocation changes are driven by `ContentSize` only, wait for frame-callback quiescence plus complete D3/Vulkan old-allocation idleness, prepare under one new generation with admission closed, then check source state and commit; an unexpected surface-format mismatch is fatal.
- [ ] Overlay close is returned as a portable event; only `Application` initiates Stop.
- [ ] Ordinary settings failure returns `AppErrorCode::PlatformOperationFailure` only after verified rollback; unverified rollback returns `AppErrorCode::RuntimeStateLost`.
- [ ] No WGC callback calls graphics code.
- [ ] Vulkan/D3 device recovery rebuilds graphics and WGC for a new generation; replica surface recovery remains inside the existing render stack and preserves WGC, D3, slots, and generation.
- [ ] Replica `SurfaceLost` retires and recreates only Vulkan surface/swapchain-dependent state, preserves WGC/D3/slots/generation, and may report started/completed in one batch for Application causal handling; `VulkanDeviceLost` alone may abandon failed Vulkan fences; confirmed D3 removal may cancel only pre-submit copies with unobserved completion, every Vulkan submit waits an already-observed D3 value, and later independent losses promote only the affected cleanup fact.
- [ ] Case 8 contains exactly seven flat groups with table loops and no nested cross-product; portable lifecycle arbitration is absent from it and remains in case 1.
- [ ] Case 8 group 7 uses one short timeline to check exactly three recovery attempts at 0/100/500 ms, successful-present-only reset, a 50 ms maximum drain poll, the 10-second no-progress Stalled edge, and progress-driven Normal reset; it does not multiply these into separate cases or nested matrices.
- [ ] One pre-publication item observation/`Closed` watcher is shared by a stable `WindowsCaptureSource` lease across every graphics/WGC job generation; no second watcher or closed-state guess exists.
- [ ] `Win32OverlayAdapter` has a `noexcept` exception-contained WndProc, normally restores it before window destruction, and on checked restoration failure keeps adapter/thunk state alive through forwarded `WM_NCDESTROY` before adapter destruction.
- [ ] Exact GLFW 3.4 is built statically with the marked no-`CS_OWNDC` main-class patch; all product windows use `GLFW_NO_API`, CMake rejects patch drift, and the adapter rejects either forbidden DC class bit before subclassing.
- [ ] Pass-through sets opaque layered attributes after style mutation, restores the exact prior layered/style snapshot transactionally, and is manually verified cross-process in bordered/borderless modes; GLFW mouse pass-through is not a second owner and `HTTRANSPARENT` alone is never sufficient.
- [ ] UI/application headers cannot access overlay, render stack, or native handles.
- [ ] No virtual Vulkan driver or generic native frame lifetime hierarchy exists.
- [ ] No commit or push is performed by this documentation-review task.
