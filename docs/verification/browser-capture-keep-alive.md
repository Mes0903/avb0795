# Browser capture keep-alive experiment

Date: 2026-09-07. Local, uncommitted acceptance record. No release operation.

## Scope and preconditions

- The user authorized the currently selected YouTube Chrome window.
- Fresh identity check: HWND `0x22067A`, PID `31880`; the supplied title matched.
- Chrome product version: `152.0.7977.82`.
- The user reports a normal browser launch without an occlusion workaround.
- A process-specific check found no matching occlusion-related launch flags. No other command-line arguments or profile contents were recorded.
- The source is not minimized. A metadata-only Z-order check found an opaque, non-layered window above it covering its visible bounds.
- No screenshots, video recordings, browser configuration changes, or unrelated captures are permitted.

## Measurement

Completed one continuous WGC capture with three 15-second legs, OFF / ON / OFF, using the production helper module. Only frame counters and memory-only regional pixel aggregates were emitted. Geometric sample regions are not detected video-player bounds.

This disposable mechanism measurement does not exercise the controller checkbox, Application command path, or production renderer. It cannot establish complete UI/runtime acceptance.

| Browser / version | Authorized source confirmed | OFF covered baseline | ON changing content | OFF baseline recovery | Taskbar / Alt+Tab | Local retirement | Result |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Chrome 152.0.7977.82 | Yes | 1 frame, then no new frames | 777 frames; sampled middle-left content changed throughout | 6 transition frames, then no new frames | Not run | Checked release confirmed | Mechanism effect observed; full application acceptance incomplete |
| Firefox | Not authorized | Not run | Not run | Not run | Not run | Not run | Not run |
| Brave | Not authorized | Not run | Not run | Not run | Not run | Not run | Not run |
| Edge | Not authorized | Not run | Not run | Not run | Not run | Not run | Not run |

## Results and limits

| Leg | Actual duration | Frames dequeued | Pixel samples | Middle-left RGB changes between samples |
| --- | ---: | ---: | ---: | ---: |
| OFF, no helper HWND allocated | 15,003 ms | 1 | 1 | No second sample |
| ON, helper activity read back as true | 15,008 ms | 777 | 8 | 7 of 7 comparisons changed |
| OFF, same hidden helper retained | 15,013 ms | 6 | 1 | No second sample |

- ON measured approximately 51.8 captured frames per second. This is the disposable WGC probe's dequeue rate, not Airelinna's presented FPS or a performance guarantee.
- The middle-left sample was uniform RGB 15 in the initial OFF leg. ON samples had mean RGB 188.59-196.45 and different fingerprints at every comparison. The top strip's fingerprint stayed unchanged throughout ON. These observations distinguish changing content from repeated frames without saving images or identifying an exact player rectangle.
- The final OFF leg received six frames during its first second and zero in every subsequent reported interval. This supports recovery of the original stopped-frame-production behavior. Its one immediate pixel sample may be a queued ON frame; final OFF appearance and pixel-for-pixel recovery were not established.
- Source identity, exact initial title, non-minimized state, and extent were rechecked continuously. Every emitted foreground observation was false. Complete coverage was checked before the run, not continuously by the probe.
- The helper remained locally active throughout ON. HIDE was read back at the OFF transition and its HWND/class remained owned for the final 15-second leg. At shutdown, `cleanup_hide_observed=1`, `uncertain=0`, and `helper_release_confirmed=true`. Cleanup then took 1 ms because the preceding hidden interval already exceeded the 250 ms grace. Local release does not itself prove browser acknowledgement.
- Total measured time: 45,032 ms. Total frames: 784. Process exit: 0. An earlier invalid-HWND/PID invocation exited 2 before WinRT initialization or capture.

Commands from the repository root:

```powershell
& 'build/keep-alive-acceptance-20260907/authorized-browser-probe.exe' 0 0
# Expected and observed exit: 2; no capture.
& 'build/keep-alive-acceptance-20260907/authorized-browser-probe.exe' 22067A 31880
# Observed exit: 0. The binary is locked to this authorization and must not be reused blindly.
```

The disposable source directly includes and links the unchanged production `BrowserCaptureKeepAlive` implementation; it does not duplicate that helper. The old diagnostic source, production sources, existing tests, and CMake files were not edited for this experiment.

Visual confirmation of exact video content, enabled/disabled tooltip interaction, repeated toggles, taskbar/Alt+Tab interference, comprehensive focus behavior, and full application shutdown remain separate acceptance checks. Membership-only access alone is not used to classify capture protection. Firefox, Brave, and Edge were not tested.
