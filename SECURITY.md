# Security and Privacy

## Capture and local data

avb0795 starts capture only after the user explicitly selects a source and
chooses **Start**. Captured pixels and source/window titles remain on the local
machine and are not transmitted by avb0795.

The executable stores `avb0795.config` beside itself using V1 JSON. When the
new file is absent, startup reads the sibling `airelinna.json`. Saves target the
new filename and preserve the legacy file. The schema contains:

- `version`
- the desired `hotkey`
- `fps_cap`
- `border`
- `extensions.browser_capture_keep_alive`, the optional browser helper preference
- `language`, the requested UI language preference (`system`, `en`, `zh-TW`, or `ja`)
- `opacity_percent`, the accepted replica opacity from 0 to 100
- `mode`, the desired input mode (`editable` or `pass_through`)
- `auto_refresh_enabled`, the source-list refresh preference

Selected sources, source/window titles, captured pixels, crop rectangles and
window geometry remain in their runtime owners. Selection's temporary input
mode and opacity overrides remain within the active gesture; persistence uses
the user's desired preferences. The executable directory requires write access
for accepted preferences to survive relaunch.

Diagnostics omit source titles and pixel contents by default. avb0795 V1 has
no telemetry, analytics, updater, network upload, or crash-reporting service.

## Windows permissions and capture exclusion

Windows controls whether a selected display or window can be captured. A
permission denial is reported to the controller and capture does not start.

avb0795's controller and replica windows use the default capturable policy, so screenshots and screen sharing can include their contents. Treat information shown in these windows as visible to your selected capture or sharing tool.

Window-source discovery filters windows owned by avb0795's current process. A full-display source still includes visible replica content on that display and can produce recursive imagery. Select a source window or place the replica on another display when capturing a display.

## Reporting a security issue

Do not attach captured content, private window titles, configuration files, or
other sensitive data to a public report. Provide the avb0795 version, Windows
version, GPU/driver information, reproduction steps, and redacted diagnostics
through the repository's private security-reporting channel when available.
