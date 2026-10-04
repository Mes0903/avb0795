# avb0795

avb0795 displays a selected window or display in an always-on-top replica window.
The Windows implementation and a direct X11 Linux implementation share the
application, UI and Vulkan renderer. Linux desktop capture and interaction
acceptance is still pending; see the [Linux development guide](docs/development/linux.md).

## Project identity

The executable and CMake project are named `avb0795`. Public C++ headers use
`avb/...` and the namespace is `avb::`. CMake options use the `AVB0795_` prefix.
On Windows, the executable stores preferences in `avb0795.config` beside itself. When that
file is absent, it reads the existing `airelinna.json`; subsequent saves use the
new filename and preserve the legacy file. The repository
directory and Git remote retain their existing locations. Linux uses
`$XDG_CONFIG_HOME/avb0795/avb0795.config`, or `$HOME/.config/avb0795/avb0795.config`
when XDG_CONFIG_HOME is absent or relative. Historical planning
and verification records retain the names used when those records were created.

## Controller window

The controller opens as a single borderless native window filled by ImGui. Drag the title area to move it, and drag an edge or corner to resize it. The title row stays visible while the settings content scrolls.

The `_` button minimizes the controller. Restore it through the desktop shell
(the Windows taskbar on Windows). The `X` button and ordinary window close enter
the application's normal shutdown and capture-drain flow. Controller minimization
and source-window minimization have separate roles. The browser-source behavior
is described below.

Linux uses the ImGui source catalog: Refresh, select a window or display, then
Start. No system sharing dialog is required. Controller and replica windows use
GLFW X11. On GNOME Wayland, XWayland permits sources from the same X server; native
Wayland windows and the complete Wayland desktop are deferred. Both devices require Vulkan 1.3 and enabled
`VK_KHR_swapchain_maintenance1` or `VK_EXT_swapchain_maintenance1` present fences.
Linux global hotkeys, capture exclusion and browser keep-alive are disabled in
this first version. Use the controller to restore Editable or Stop after
PassThrough or opacity 0. Genuine GNOME Xorg capture, background Chrome and the full desktop operation
matrix remain unverified.

## Replica opacity and source region

Use the **Opacity** slider to select a 0–100% value, or Ctrl-click the number
to enter a percentage. Accepted opacity is saved when the edit finishes.

During capture, **Select region** shows the full source temporarily at 100%
opacity in Editable mode. Drag a rectangle on the replica to crop it; Esc or
Cancel restores the previous crop. **Restore full source** returns to full view.
Crops use source pixels and remain with
the current session across Stop/Start. Source selection resets the crop.

操作與資料流說明：[框選與不透明度操作](docs/development/region-selection.md)。

## 通用偏好保存

為了接續上次的操作習慣，程式會將語言、快捷鍵、FPS、邊框、可編輯／穿透模式、
browser keep-alive、自動重新整理與不透明度保存到 `avb0795.config`
Windows 使用執行檔旁的路徑，Linux 使用上述 XDG 路徑
內容為 V1 JSON，設定目錄需要具備寫入權限。 Linux 保留停用能力的偏好

離散選項在接受後保存，不透明度在拖曳或數值輸入完成後保存。下次啟動讀回這些
偏好，來源與擷取範圍由當次操作設定；視窗位置與大小採原有啟動配置。
保存失敗時控制面板會顯示診斷，已接受的設定持續生效，後續設定操作可重試保存。

## Screenshots and screen sharing

The controller and replica windows are capturable by default. Use your usual screenshot or screen-sharing tool to include them. On Windows, window-source discovery filters windows owned by avb0795's current process. On Linux, the catalog excludes the exact owned controller and replica drawables; other windows from the same process remain eligible.

When capturing an entire display, a replica on that display can appear inside its own captured image and produce a recursive mirror. Select a source window or place the replica on another display to keep that feedback out of the captured area.

## Localization

The controller supports English, Traditional Chinese, and Japanese. Choose a
requested language under **General settings > Language**. The default is
**Follow system** (`system` in the configuration), and an accepted manual choice
changes the controller immediately and starts a save of the requested preference.
A failed save reports a notice and leaves the in-memory choice active so a later
accepted preference command can retry it.

On Windows, when **Follow system** is active, avb0795 queries the ordered Windows UI
language preferences at startup and queries them again when **Follow system** is
explicitly selected. It does not watch for background OS language changes. The
first supported language family wins: `en` selects English, every `zh` family
tag selects Traditional Chinese, and `ja` selects Japanese. If none is supported
or the query fails, avb0795 uses English. Linux follows its effective locale and
ordered LANGUAGE preferences; C/POSIX or unsupported languages select English.

Changing the language affects presentation only and does not reset an active
capture. Automated application tests cover that state contract. Real-window
language cycling, DPI behavior, unfinished hotkey edits, and relaunch persistence
still require manual UI acceptance.

## Browser capture limitations

**Continuous live capture of a truly minimized browser window is not supported.**
Minimizing the source to the taskbar is different from making its window smaller
or covering it with another window.

| Source state | What to expect |
| --- | --- |
| Resized to a smaller window | Resizing alone is not the minimized-window limitation. |
| Fully covered, but not minimized | Windows experimental keep-alive may help; Linux requires direct source/motion verification. |
| Minimized to the taskbar | Live updates are not supported; a frozen frame or cached thumbnail is not live video. |

### Experimental browser keep-alive

On Windows, in the controller/configuration window, under **Extensions**, enable
**Browser capture keep-alive (experimental)**. Hover over **(?)** for details.
The option is off by default and applies only to window capture, not display
capture.

Keep the source browser window open and **not minimized**, with the video tab
selected. It may be covered by other windows while using this workaround.

The feature operates an avb0795-owned helper window. It does not move, resize,
restore, or change the styles of the source browser window; it does not modify
browser settings, require a browser restart, or install a browser extension.

- It may also keep other browser windows rendering, increasing CPU/GPU use and
  power consumption.
- Inactive tabs, protected video, and locked or sleeping displays are not
  guaranteed to work.
- Browser updates may break the workaround. Returning to normal browser
  throttling after disabling it is best-effort.
- An enabled checkbox indicates the accepted preference, not proof that new video
  frames are arriving.

### Why minimizing is different

The mechanism below describes Windows. Linux also makes no promise of live
capture from a truly minimized source; its disabled keep-alive does not change
Chrome playback, settings or flags.

Chromium hides its internal root window when the native window is minimized.
The task-preview exception used by this helper does not override a hidden root.
See Chromium's [minimize/restore handling](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/views/widget/desktop_aura/desktop_window_tree_host_win.cc)
and [occlusion tracker](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/aura/native_window_occlusion_tracker_win.cc).
Microsoft's [Windows Graphics Capture sample](https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/blob/master/cpp/ScreenCaptureforHWND/README.md)
also notes that minimized windows are enumerated but not captured.

There is currently no known reliable avb0795-only workaround that provides
continuous new video frames while leaving an existing browser window truly
minimized and leaving the browser and its window unchanged. This is a current
support limitation, not a claim that every possible future technique is
impossible.

## Developer documentation

- [Architecture overview and subsystem diagrams](docs/architecture/README.md)
- [Localization architecture](docs/architecture/localization.md)
- [clangd setup](docs/development/clangd.md)
- [Linux development and desktop acceptance](docs/development/linux.md)
