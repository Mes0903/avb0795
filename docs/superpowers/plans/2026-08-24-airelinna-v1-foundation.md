# Airelinna V1 Foundation Implementation Plan

> **For agentic workers:** Implement this plan before the Windows and rendering plans. This document does not authorize a commit or push.

**Goal:** Establish the portable domain, application ports, bounded primitives, configuration, build graph, and four high-value deterministic tests without introducing native-resource abstractions.

**Architecture:** `core` owns values and policies. `app` owns ports and use-case-facing commands. Neither layer contains Win32, D3D, Vulkan, GLFW-native, or type-erased resource handles.

**Tech Stack:** C++23, CMake 3.28+, nlohmann/json, Catch2.

---

## 1. Files and responsibilities

| Path | Responsibility |
|---|---|
| `CMakeLists.txt`, `cmake/Dependencies.cmake` | Options, pinned dependencies, target ordering |
| `include/airelinna/core/types.hpp` | Portable identifiers, settings, errors, capabilities, hotkey values |
| `include/airelinna/core/replica_session.hpp` | One session state machine |
| `include/airelinna/core/application_state.hpp` | One current session plus stale-command generation token |
| `include/airelinna/core/latest_value_mailbox.hpp` | Bounded newest-only move transfer |
| `include/airelinna/core/frame_scheduler.hpp` | FPS deadline policy |
| `include/airelinna/app/config_store.hpp` | Portable application persistence using one atomic-replace callable |
| `src/app/config_store.cpp` | Concrete config validation and persistence implementation |
| `include/airelinna/app/commands.hpp` | Frontend-to-application command values |
| `include/airelinna/app/ports.hpp` | Three platform ports and runtime events |
| `tests/core/*` | Named cases 2–5 only |
| `cmake/VerifySourceBoundaries.cmake` | One dependency-direction gate |

Do not create portable `CapturedFrame`, `RenderFrame`, `IFrameBridge`, renderer interface, window interface, or platform-name enum.

## 2. Task 1: Create a buildable target graph

**Files:** root `CMakeLists.txt`, `cmake/Dependencies.cmake`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`.

- [ ] Define options `AIRELINNA_BUILD_TESTS` and `AIRELINNA_ENABLE_WINDOWS_INTEGRATION`; the latter is off unless explicitly enabled.
- [ ] Pin nlohmann/json and one exact Catch2 3.x release whose bundled CMake helper supports both `ADD_TAGS_AS_LABELS` and `SKIP_IS_FAILURE`; disable every dependency's own tests. GLFW, Dear ImGui, and Vulkan are added by P3 when their first source files exist.
- [ ] Create `airelinna_core` only after its first source exists. Create `airelinna_app` when Task 5 adds `config_store.cpp`; link `airelinna_core` publicly and nlohmann/json with the narrowest visibility. Do not create a source-less test executable in this task; Task 4 creates `airelinna_core_tests` together with the first case-2 source and then registers it once with `catch_discover_tests(... ADD_TAGS_AS_LABELS)`.
- [ ] Use `${PROJECT_SOURCE_DIR}/include`; never use `/include`.
- [ ] Put all Windows target creation and later `target_sources` calls inside `if(WIN32)`.
- [ ] Configure with ClangCL and verify the empty foundation graph succeeds before adding domain sources.

Canonical command:

```powershell
cmake -S . -B build/clang-debug -G 'Visual Studio 17 2022' -T ClangCL -DAIRELINNA_BUILD_TESTS=ON
cmake --build build/clang-debug --config Debug
```

Expected: configure and build succeed. No step adds a not-yet-created source to force a synthetic CMake failure.

## 3. Task 2: Define portable values and state

**Files:** `core/types.*`, `core/replica_session.*`, `core/application_state.*`, `app/commands.hpp`.

### 3.1 Value rules

Define these portable values:

```cpp
struct SessionId { std::uint64_t value{}; };
struct SourceId { std::string value; };
struct Extent { std::uint32_t width{}; std::uint32_t height{}; };

enum class SourceKind { Display, Window };
enum class SessionLifecycle { Idle, Starting, Capturing, Stopping, Interrupted, RecoveringGraphics, Error };
enum class OverlayMode { Editable, PassThrough };
enum class BorderPreference { Bordered, Borderless };
enum class FpsCap { Fps30, Fps60, Fps144, Unlimited };
enum class ColorSupport { Sdr, HdrDetectedUnsupported, UnknownAssumeSdr };
enum class ExitDisposition { Continue, Clean, AbandonProcess };
enum class CaptureExclusionStatus { NotAttempted, Excluded, Unsupported, Failed };
enum class RuntimeDrainStatus { Normal, Stalled };
enum class BridgeMode { CpuCopy, GpuInterop };
enum class BridgeReason {
  None,
  AdapterLuidUnavailableOrMismatch,
  ExternalImageUnsupported,
  RequiredD3d11InterfacesUnavailable,
  SharedFenceOrTimelineSemaphoreUnavailable,
  TimelineFenceImportUnavailable,
  Win32ImportProcedureUnavailable,
  CompatibleMemoryTypeUnavailable,
  InteropOperationFailure,
};

enum class AppErrorCode {
  InvalidCommand,
  InvalidValue,
  ConfigReadFailure,
  ConfigWriteFailure,
  SourceCatalogFailure,
  PermissionDenied,
  SourceUnavailable,
  HotkeyConflict,
  HotkeyCleanupFailure,
  HotkeyRegistrationLost,
  UnsupportedWindowClass,
  UnsupportedGraphics,
  RuntimeConstructionFailure,
  PlatformOperationFailure,
  RuntimeStateLost,
};
```

`SourceDescriptor` contains ID, kind, display label, extent, and color support. `ReplicaSettings` contains mode, border, and FPS cap. `PlatformCapabilities` contains only static behavior that changes visible controls before a runtime exists: display capture, window capture, pass-through, borderless resize, global hotkey, and capture exclusion. GPU bridge availability is deliberately absent because per-handle D3D/Vulkan compatibility is known only while constructing a concrete runtime. `BridgeReason::None` is valid only with `GpuInterop`; initial capability fallback carries the first failed capability reason in declaration order, while `InteropOperationFailure` is reserved for import/setup or non-device-lost operational fallback after those facts passed. `ExitDisposition` is a host-facing Application value, not a session lifecycle: `AbandonProcess` means main must terminate without unwinding native owners, and P4 is its sole decision owner. These are portable values, not a bridge interface or policy object.

`HotkeyChord` contains typed modifier flags and one supported key code. Parsing returns `std::expected<HotkeyChord, AppError>`; formatting/JSON serialization derives from the typed value. Empty chords, modifier-only chords, duplicate modifiers, and unsupported keys are rejected before a platform registration call.

`AppError` is an allocation-free value containing one canonical `AppErrorCode` plus an optional fixed-width numeric native detail. Its user message and diagnostic label are derived from the code as static `std::string_view` values; it owns no dynamic text. Rich adapter context may be written separately to the diagnostic sink, but it never becomes ownership inside a no-throw port result. Its `retryable()` value is derived from one `constexpr` mapping rather than stored independently: config I/O, catalog/permission/source availability, hotkey conflict/cleanup/lost-registration, runtime construction, and ordinary platform-operation failures are retryable; invalid command/value, unsupported window/graphics, and `RuntimeStateLost` are not retryable as the same operation. `RuntimeStateLost` means a published runtime can no longer continue reliably and requires teardown: this includes an unverified settings rollback, terminal capture/graphics failure, terminal Vulkan OOM/unknown result, or exhausted device-recovery budget. Native HRESULT/`VkResult` values are translated at adapter boundaries and may occupy only the optional numeric detail.

The mapping is normative and exhaustive: config operations use `ConfigReadFailure`/`ConfigWriteFailure`; enumeration and source identity use `SourceCatalogFailure`/`SourceUnavailable`; consent uses `PermissionDenied`; hotkey transactions use their three dedicated codes; forbidden class or mandatory graphics-feature absence uses `UnsupportedWindowClass`/`UnsupportedGraphics`; other pre-publication runtime construction uses `RuntimeConstructionFailure`; a published native operation that restores and verifies its old state uses `PlatformOperationFailure`; every terminal condition of the current published runtime uses `RuntimeStateLost`. Source closure remains the non-error `SourceLost` lifecycle event. Later phases must use these exact members rather than add local error-code enums; their existing behavioral cases assert the relevant code and retryability without a standalone error-struct test.

### 3.2 State ownership

`ReplicaSession` owns state values only. Its transition methods validate lifecycle changes and return `std::expected<void, AppError>`. It never calls a backend.

`ApplicationState` owns exactly one current `ReplicaSession` and its monotonically replaced `SessionId` generation token in V1; it does not pre-build a collection or one-active arbitration layer. Its transition methods return errors to the owning `Application`; remove `last_error(SessionId)` and any mutable error slot from domain state. `Application`, not `ApplicationState`, later owns one optional application-level notice for non-session failures.

`AppCommand` is a `std::variant` of refresh sources, select source, start, stop, set mode, set border, set FPS, replace hotkey, session-agnostic `RestoreActiveReplicaEditable`, and quit. Every session-specific command carries the projected `SessionId`; `Application` rejects it before mutation when it no longer matches the current scalar session, preventing queued UI work from affecting a replacement. The restore command intentionally carries none so a platform hotkey adapter never needs application/session state.

Application-level lifecycle verification is case 1 in P4, where real port calls and domain transitions can be checked together. Do not duplicate it with a state-only test here.

## 4. Task 3: Define the application ports

**Files:** `include/airelinna/app/ports.hpp`.

Keep exactly three independent platform service ports plus the runtime returned by its factory:

```cpp
inline constexpr std::size_t max_runtime_events_per_tick{8};

struct RuntimeTickResult {
  std::array<std::optional<RuntimeEvent>, max_runtime_events_per_tick> events{};
  std::optional<std::chrono::steady_clock::time_point> next_wake;
};

class ISourceCatalog {
public:
  virtual ~ISourceCatalog() = default;
  virtual std::expected<std::vector<core::SourceDescriptor>, core::AppError> enumerate() = 0;
};

class IReplicaRuntime {
public:
  virtual ~IReplicaRuntime() = default;
  virtual std::expected<void, core::AppError> apply(
      const core::ReplicaSettings&) noexcept = 0;
  virtual RuntimeTickResult tick(
      std::chrono::steady_clock::time_point now) noexcept = 0;
  virtual void request_stop() noexcept = 0;
};

class IReplicaRuntimeFactory {
public:
  virtual ~IReplicaRuntimeFactory() = default;
  virtual std::expected<std::unique_ptr<IReplicaRuntime>, core::AppError> create(
      const core::SourceDescriptor&, const core::ReplicaSettings&) = 0;
};

class IGlobalHotkey {
public:
  virtual ~IGlobalHotkey() = default;
  virtual std::expected<void, core::AppError> replace(const core::HotkeyChord&) = 0;
  virtual void clear() noexcept = 0;
};
```

`RuntimeEvent` is a portable variant: `BridgeModeChanged { mode, reason }`, overlay capture-exclusion status, `RuntimeDrainStatusChanged { Normal | Stalled }`, overlay close requested, source lost, graphics-recovery-started, graphics-recovered, stopped, or `Failed { core::AppError }`. There is no second degraded-to-CPU event updating the same state. Drain status covers the whole native runtime drain, including callback/job cleanup and graphics retirement; it is diagnostic only and never grants release permission. `RuntimeTickResult` uses eight fixed optional slots and an optional `steady_clock` wake deadline, so producing, copying, or returning the batch never allocates. Eight covers the three status-only event kinds plus all five distinct pre-stop lifecycle candidates; a safe post-stop `Stopped` is returned alone. Add compile-time nothrow-copy and nothrow-move assertions for `AppError`, `RuntimeEvent`, and the result. Events carry no frame, `HWND`, texture, Vulkan handle, callback token, or cleanup function.

Bridge, exclusion, and drain-status changes are status-only. Before runtime stop is requested, the runtime reports every distinct lifecycle candidate it observed and does not rank them. `Application` is the only owner of `Failed > OverlayCloseRequested > SourceLost > recovery`, independent of fixed-slot order. A recovery outcome applies `GraphicsRecoveryStarted` and then `GraphicsRecovered` when both causal facts occur in one batch; this pair is one winning outcome and lower-priority candidates are still ignored. After runtime stop is requested, the runtime's native resource phase suppresses lifecycle candidates except `Stopped`; safe `Stopped` is returned alone. `Application` defensively uses its own private `stop_requested` bit rather than inferring this phase from the visible session lifecycle, which may remain Error during terminal teardown.

`tick(now)` advances nonblocking runtime work, including callback quiescence and graphics retirement after `request_stop()`, and synchronously returns feedback plus the earliest runtime wake deadline. It may return no deadline only when no internal work is pending and an external event is required; every incomplete stop/reconfigure/recovery/drain contributes a strictly future bounded observation deadline even if no callback or graphics event can wake the loop. `request_stop()` closes callback admission/revokes handlers, starts draining, and returns without waiting for callbacks or fences; only a later `Stopped` event permits destruction of the published runtime. This is the only timing/feedback path exposed by the runtime port. There is no callback-held application event sink, event queue, or runtime epoch. `apply` is all-or-nothing for ordinary failures; an unverified rollback returns terminal `RuntimeStateLost`. Both virtual operations contain every native/projection/allocation exception and can construct their fixed `AppError` fallback repeatedly without allocation. Before stop, a `tick` catch clears the partial batch, returns only `Failed { RuntimeStateLost }`, and retains a bounded wake deadline while the published runtime still owns resources. During stop, the same catch emits no lifecycle failure: it latches drain uncertainty, retains every owner, and returns the bounded observation deadline needed to reach `RuntimeDrainStatus::Stalled` or later prove safe `Stopped`.

No test is added merely to prove these interfaces inherit or compile. Their use is covered by the state/controller and Windows runtime cases.

## 5. Task 4: Add bounded mailbox and scheduler

**Files:** `latest_value_mailbox.hpp`, `frame_scheduler.*`, tests for cases 2 and 3.

When the case-2 source first exists, create `airelinna_core_tests` with that source, link Catch2, and call `catch_discover_tests(... ADD_TAGS_AS_LABELS)` in the same task. Add case 3 to that existing target afterward; later plans use `target_sources` only after their files exist.

### 5.1 Newest-only mailbox

Implement a mutex-protected optional value with:

- `submit(T)` returning `BecameReady`, `ReplacedPending`, or `Closed` while replacing an unpublished value when present;
- `take()` moving out the newest value;
- `close()` destroying pending content and rejecting later submissions;
- counters for submitted, replaced, and taken values.

Move replaced/closed content out while holding the mutex and destroy it after unlocking, so a WGC checkout close never runs under the mailbox lock. The mailbox is not a general queue, has no blocking wait, and stores at most one value. `BecameReady` lets a producer wake only on empty-to-ready transition; replacement alone does not cause a capped-FPS wake storm. Waking the application loop remains the producer's separate responsibility.

Test case 2 uses a move-only counted value and checks all three submit results, replacement destruction outside the lock, take ownership, close destruction, and rejected post-close submission in one case.

### 5.2 FPS scheduler

Use `std::chrono::steady_clock`. 30/60/144 caps advance monotonic deadlines and skip missed periods rather than burst. Unlimited permits work only when a new frame or redraw exists; it is not a busy-spin mode.

Test case 3 is table-driven for all four caps, includes one missed-deadline jump, and checks no catch-up burst.

## 6. Task 5: Add config and hotkey value validation

**Files:** `config_store.*`, `hotkey.*`, tests for cases 4 and 5.

`UserConfig` stores only version, `HotkeyChord`, FPS cap, and border preference. It contains no source ID, title, overlay position, or window size.

`ConfigStore` is concrete. Its constructor receives one `AtomicReplaceFn` callable for the platform-specific final replacement. Do not create a virtual filesystem or `IAtomicFileReplacer` hierarchy. Save writes a sibling temporary file, flushes and closes it, invokes the replacement callable, and removes the temporary file on failure. This synchronous operation has no wall-clock timeout guarantee, so only accepted settings commands invoke it; shutdown never retries a dirty save before native drain.

Test case 4 covers missing-file defaults, valid round trip, unknown version/default recovery, invalid hotkey/default recovery, exact allowed JSON keys, and one replacement-callable failure using one temporary directory. The failure section verifies the old destination is unchanged, the sibling temporary file is removed, and `ConfigWriteFailure` is returned.

Test case 5 covers typed chord validation and canonical serialization. Platform registration rollback belongs to Windows case 10, not this portable test.

## 7. Task 6: Add the architecture boundary gate

Create one `cmake/VerifySourceBoundaries.cmake` module with two parts, not another test framework:

- a configure-time `airelinna_assert_target_boundaries()` function, called after P4 creates the final targets, that reads each target's `SOURCES`, `LINK_LIBRARIES`, and `INTERFACE_LINK_LIBRARIES` and enforces exact source-root/direct-link allowlists;
- the existing `verify_source_boundaries` custom target, which scans `include/airelinna/core`, `src/core`, `include/airelinna/app`, `src/app`, `include/airelinna/ui`, `src/ui`, `include/airelinna/desktop`, `src/desktop`, `platform/windows`, `render`, and `main.cpp`. It may tolerate an absent later-phase path but must scan it once it exists.

It fails when:

- portable files include Win32, WGC, D3D, DXGI, Vulkan, GLFW-native, or platform/windows headers;
- UI files include any application header except `app/commands.hpp` and `app/controller_view_state.hpp`, or mention `Application`, `IReplicaRuntime`, `IGlobalHotkey`, `ConfigStore`, `OverlayWindow`, `WindowsRenderStack`, `HWND`, or native graphics types;
- desktop-shell files include any application header except `app/application.hpp`, `app/commands.hpp`, and `app/controller_view_state.hpp`; directly include `app/ports.hpp` or mention a port, replica runtime, `ConfigStore`, overlay, WGC/native-capture type, or replica render type. The portable `CaptureExclusionStatus` value plus controller-only GLFW/Vulkan infrastructure remain explicitly allowed;
- Windows adapter files include `app/application.hpp`, `core/application_state.hpp`, `core/replica_session.hpp`, any UI/DesktopHost header, or mutate domain/session state; their application surface is limited to portable values, `app/ports.hpp`, and `app/commands.hpp` where the hotkey emits a command;
- render files include Application/domain/UI/DesktopHost headers, while `main.cpp` contains session decisions, runtime operations, capture/render recovery, or frontend rendering beyond concrete construction, `DesktopHost::run`, exit-disposition handling, and process setup/teardown;
- `airelinna_ui` has a source outside UI roots or a direct link outside `airelinna_core` plus Dear ImGui; `airelinna_desktop` has a source outside DesktopHost/controller-backend roots or directly links a Windows capture/replica-graphics target; portable targets directly link platform/UI/Desktop targets; or any non-executable target aggregates all concrete layers;
- removed generic frame contracts or `void* native_resource` reappear.

Configure fails immediately on target-graph violations; register the text scan as the single `verify_source_boundaries` target, not as dozens of header-level tests.

## 8. Foundation completion gate

- [ ] Build graph configures before and after foundation sources are added.
- [ ] `airelinna_app` exists before P2 and owns the concrete `ConfigStore` implementation plus the portable port headers.
- [ ] Exactly named cases 2-5 exist at this phase and pass; no assertion cluster is split into another top-level case.
- [ ] Domain and commands contain values only.
- [ ] Application ports expose no native resources; the fixed `AppErrorCode` mapping and Application-owned `ExitDisposition` remain portable values rather than new service interfaces.
- [ ] `ApplicationState` returns command errors directly.
- [ ] Mailbox owns at most one move-only value and destroys replaced values.
- [ ] Config schema has exactly version/hotkey/fps/border.
- [ ] Configure-time source/direct-link allowlists and the single `verify_source_boundaries` text-scan target both pass over every layer that exists at the current phase.
- [ ] The exact Catch2 pin supports `ADD_TAGS_AS_LABELS` and `SKIP_IS_FAILURE`, and dependency-owned tests are disabled.
- [ ] `git diff --check` passes for tracked changes and the master plan's explicit read-only scan checks all six untracked/working-tree Markdown documents; no staging, commit, or push is performed by this plan-review task.
