# Security and Privacy

## Capture and local data

Airelinna starts capture only after the user explicitly selects a source and
chooses **Start**. Captured pixels and source/window titles remain on the local
machine and are not transmitted by Airelinna.

The executable stores `airelinna.json` beside itself. Config V1 contains only:

- `version`
- the desired `hotkey`
- `fps_cap`
- `border`

It does not persist a selected source, source or window title, captured pixels,
window position, or the session-only Editable/Pass-through mode.

Diagnostics omit source titles and pixel contents by default. Airelinna V1 has
no telemetry, analytics, updater, network upload, or crash-reporting service.

## Windows permissions and capture exclusion

Windows controls whether a selected display or window can be captured. A
permission denial is reported to the controller and capture does not start.

Airelinna requests OS capture exclusion for its controller and replica windows,
but Windows treats this protection as best effort. Unsupported or failed
exclusion is shown as a warning; it is not a security boundary and cannot
guarantee that every capture tool or environment will omit the windows.

## Reporting a security issue

Do not attach captured content, private window titles, configuration files, or
other sensitive data to a public report. Provide the Airelinna version, Windows
version, GPU/driver information, reproduction steps, and redacted diagnostics
through the repository's private security-reporting channel when available.
