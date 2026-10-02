# Experimental browser capture keep-alive

Date: 2026-09-07
Status: Proposed details; awaiting user review before implementation.

## Goal and scope

Add an opt-in, built-in extension that attempts to keep a fully covered,
non-minimized browser window rendering during Airelinna window capture.
The user enables it from the existing controller/configuration window without
editing browser flags, restarting the browser, or installing a browser extension.
This is an experiment, not a guarantee for every browser or protected video.

The existing WGC capture, frame bridge, renderer, capture exclusion, and source
identity contracts remain unchanged. No dynamic plugin loader, generic extension
registry, browser injection, profile edits, elevated service, or release work.

## Approach and trade-offs

Use one isolated Windows compatibility module and a small portable preference.
The proposed module owns a non-activating, visually unobtrusive helper window
whose show/hide events exercise the browser's task-preview occlusion exception.
It does not change the selected browser window's styles, position, or process.

Alternatives already discussed are browser launch flags (require browser setup
or restart) and a browser extension/tab-capture path (requires installation and
a separate capture pipeline). Neither meets the requested first experiment.
A general plugin framework adds no needed capability for this single feature.

Chromium currently recognizes `TaskListThumbnailWnd` / `MultitaskingViewFrame`
show/hide events and applies the exception across its tracked root windows.
Consequently, this helper may also keep other browser windows rendering and
increase CPU/GPU use and power consumption. It is not source-local isolation.
The mechanism relies on browser internals, not a supported browser API.
See [Chromium's native occlusion implementation](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/aura/native_window_occlusion_tracker_win.cc).

## Configuration UI

Add an `Extensions` section with this English control:

`[ ] Browser capture keep-alive (experimental)  (?)`

The checkbox defaults to off. The adjacent `(?)` shows this wrapped tooltip on
hover, including when the control is temporarily disabled:

> Attempts to keep covered browser windows rendering during window capture.
> May also affect other browser windows and increase CPU/GPU usage and power
> consumption. Does not change browser settings. Minimized windows, inactive
> tabs, protected video, and locked or sleeping displays are not guaranteed.
> Browser recovery after disabling is best-effort. Experimental: browser updates
> may break this workaround.

Allow live toggling for A/B testing when settings commands are allowed. While
idle, toggling changes only the saved preference; no helper is active. A display
capture never activates the helper. Show `Window captures only` where relevant.
Disable mutation during stopping/shutdown and on unsupported backends, using
portable capability/view-state values rather than platform-name checks in UI.
The checkmark means an accepted preference, not proof that browser frames update.

Persist only the boolean preference, never browser identities or window data.
Extend the V1 config parser to accept both the existing four-key document and an
optional, strictly validated `extensions` object containing only
the required boolean `browser_capture_keep_alive`. An absent `extensions`
object means false. Preserve
existing hotkey/FPS/border settings when loading an old document. Retain atomic
save and the existing malformed-config behavior. Old binaries are not promised
to read the extended document: their existing strict parser falls back to defaults.
Keep the preference through application initialization and session replacement.

## Layer boundaries

| Layer | Responsibility |
| --- | --- |
| Controller UI | Draw checkbox/help; emit a typed application command. No native calls. |
| Application | Validate the session/control state; apply the requested settings before committing and persisting them. |
| Portable settings/view state | Carry a boolean and capability/control availability only; no browser mechanism or native identity. |
| Windows runtime | Own the compatibility module; coordinate its enable/disable with runtime lifetime. |
| `platform/windows/features/browser_capture_keep_alive.*` | Own helper window/class/events and cleanup; contain all browser-specific constants and workaround policy. |

Keep extension preferences grouped in a small value type carried by the existing
settings path. Do not add another application port or bypass `runtime.apply`.
No renderer, WGC callback, or desktop-host code depends on the feature module.
Main-thread ownership follows the project's existing native-window contract.
The helper is C++-owned by the module, not a Win32 child/owned window of the
controller or overlay; tearing those windows down must not shorten its lifetime.

## Activation, errors, and cleanup

- Prepare the requested helper hidden as part of window-runtime startup, before
  the final capture activation. Do not publish a helper SHOW before the runtime
  has been successfully published. Failed construction can therefore destroy the
  never-shown helper without leaving a browser occlusion exception behind.
  First activation occurs in the published runtime's first tick, which Start
  already schedules; a stop/source-loss/fatal observation takes precedence.
- Live transactions and first-tick activation verify only Airelinna-owned state:
  accepted preference, helper identity/visibility, and any local registrations.
  They cannot verify that a browser consumed an event or resumed its video.
  Browser inactivity alone is not `RuntimeStateLost`.
- Live enable/disable participates in the existing settings transaction. An
  ordinary failure preserves the previous accepted preference and native state
  and reports an application notice. Unverifiable rollback uses the existing
  `RuntimeStateLost` safe-stop path, not a silently incorrect checkmark.
- A requested helper-creation failure rejects startup with an error. The user
  can disable the experimental preference and use ordinary capture.
  A local first-tick activation failure uses the published runtime's existing
  `Failed{RuntimeStateLost}` event and safe-stop path, preserving the distinction
  from an unpublished factory error. No fallible step follows StartCapture
  inside factory publication.
- Stop, source loss, and normal exit deactivate the helper promptly, independently
  of potentially lengthy GPU drain. A brief graphics recovery may retain it only
  while the same source remains connected and stop has not been requested.
  Changing a preference on an interrupted runtime must not reactivate the helper;
  the new value takes effect when a connected window capture is started.
- Cleanup separates immediate deactivation from resource retirement as specified
  below. It is idempotent and does not depend on destructor execution alone;
  `request_stop()` also initiates it on the existing `AbandonProcess` path.
  No browser-setting restoration is needed because no settings are edited.
- Real taskbar previews and Alt+Tab can clear the browser exception. Check this
  during the experiment; any necessary reassertion must stay inside the module,
  avoid taking focus, and stop entirely when disabled. No unconditional busy loop.

### Deactivation versus helper retirement

Chromium installs out-of-context WinEvent hooks and looks up the helper's class
name while handling HIDE. Those callbacks are asynchronous: immediately destroying
the helper can invalidate that lookup before the browser handles the event.
`ShowWindow`/`NotifyWinEvent` returning, or observing our own hook, is not a browser
acknowledgement. See [Microsoft's WinEvent delivery contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook).

- On live disable, source loss, or rollback to disabled, stop any reassertion,
  hide the helper, and retain its valid hidden window/class within the module.
  Reuse that single window for a later enable; do not accumulate retired helpers.
- On runtime stop, hide immediately and schedule final retirement through the
  existing `tick(now)`/`next_wake` path. Start with a named 250 ms grace interval
  measured from the latest actual HIDE transition. Already-hidden helpers whose
  interval elapsed need no new delay; never-shown helpers need none at all.
  No sleeps, nested message pump, worker thread, or browser IPC is introduced.
- Advance helper retirement independently of GPU drain, on the owner thread.
  Include pending local helper ownership in the runtime's existing cleanup and
  `Stopped` conditions; do not release the module while its window or callback
  bindings can still refer to it. A known local cleanup failure follows the
  existing retained-owner/stalled-drain policy, not a false `Stopped` event.
- The grace interval only reduces the event-delivery race; it is not proof of
  browser recovery. Recovery on normal disable/stop/exit, rollback after SHOW,
  and forced termination is explicitly best-effort. `AbandonProcess` may bypass
  the grace interval. Do not wait indefinitely for browser state we cannot read,
  and do not expand into a global browser-state coordinator or watchdog service.
- A successful local rollback restores our own helper/settings snapshot, not an
  exact browser-global snapshot. Taskbar previews and other compatibility tools
  can change that external state independently. Concurrent occlusion workarounds
  are outside this experiment's reliable A/B baseline.

## Verification and acceptance

Extend existing suites with a small set of focused scenarios: legacy/default
config and round trip; command/view projection; rejected/stale commands;
transaction rollback; inactive/display capture behavior; helper startup unwind;
repeat toggle/stop/source-loss cleanup, including hidden-window retention,
retirement deadlines, and suppression of SHOW on failed startup or pending stop.
Use controllable time in the existing native-policy tests for retirement; a test
timer or our own event hook cannot prove a real browser consumed HIDE.
Do not create a large mock framework or
dozens of standalone test cases. Run source-boundary checks and non-hardware tests.

For the actual effect, use the user's authorized Chrome video window only:

1. Confirm the source identity and that the test browser is not already running
   with occlusion-disabling flags; otherwise an off/on comparison is inconclusive.
2. Compare helper off/on/off while that window is fully covered, not minimized,
   with the same active video tab. Measure frame arrivals and visually check video
   motion; frame counts alone do not prove non-black, changing video content.
3. Exercise repeated checkbox toggles, real taskbar previews, and Alt+Tab; check
   focus/input/taskbar behavior and absence of a visible helper artifact.
4. Check the off leg while capture remains active, then stop capture and close
   Airelinna normally. Verify local helper retirement separately from observed
   browser return to the baseline. Missing baseline recovery is a failed or
   inconclusive experiment, not a pass based on successful native cleanup.
   Recheck ordinary capture
   with the preference off and display capture with no helper activation.

Do not run broad desktop-capture hardware tests or retain screenshots/video.
Report any browser-effect or cleanup limitation separately from build/test success.
No commits are requested; tests, specs, plans, and work records remain uncommitted.
