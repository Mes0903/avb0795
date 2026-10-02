# Other Issue

## Case 12 owner-order coverage

- Severity: Low
- Status: Deferred
- Found by: Rolling review round 3
- Area: `tests/windows/windows_policy_test.cpp`

Case 12 currently uses a small fake owner containing a pool-lifetime token and
`LatestValueMailbox<FakeCheckout>` to verify destruction order. The `Normal`
and `Reconfigure` branches are effectively identical and do not execute the
production `WgcCaptureJob` teardown or reconfiguration wiring.

Concrete risk: a future change could destroy the production frame-pool owner
before releasing a mailbox-held or render-held checkout while this isolated
fake test continues to pass.

This was not fixed during P2 because adding a production-only test seam solely
for this assertion would increase coupling and overdesign the capture API. The
current production member order provides the structural lifetime guarantee,
while the hardware case exercises the real normal and terminal-stop paths.

Revisit when P3 introduces a natural production-owned runtime handoff or
teardown boundary. At that point, replace or remove the fake owner-order test
and verify normal, reconfigure, and terminal-stop lifetime through that real
boundary without exposing native ownership to frontend or application layers.

## 副本視窗初始尺寸偶發過小

- 日期：2026-09-19
- 狀態：待使用者提供重現步驟；保留現有初始尺寸行為。
- 現象：使用者選取來源並開始擷取後，產生的 replica 視窗偶爾非常小，曾見於 WireGuard 與 Slack。問題對象是副本視窗。
- 最新回報：使用者後來觀察到現象已消失，將於再次發生時補充重現步驟。
- 本輪操作：完成程式碼追查與唯讀幾何量測。初始尺寸 production code 維持原樣；目前證據支持「本次觀測正常」，修復狀態仍待重現與驗證。
- 桌面操作偏好：後續使用程式碼檢查與非互動測試；真實來源視窗的重現由使用者提供。

### 本次 WireGuard 觀測

| 欄位 | 值 |
|---|---|
| HWND | 65650 / 0x10072（當次 process-local 身分） |
| IsIconic | 0 |
| DPI | 96 |
| GetWindowRect | 675×525 |
| GetClientRect | 659×486 |
| Normal placement | 675×525 |
| DWM frame bounds | 661×518 |
| GraphicsCaptureItem.Size | 661×518 |

量測來源：`artifacts/ux-geometry-20260919/wireguard-before.txt`。Probe 以 per-monitor-v2 座標契約讀取 WireGuard metadata 與 capture item size；影像擷取 session、連線與設定保持既有狀態。Slack 的實際幾何留待後續使用者重現。

### 目前確認的資料流程與待驗證假說

- `platform/windows/windows_source_catalog.cpp` 的 `WindowEnumerationContext::visit()` 以列舉當下的 `GetWindowRect` 形成 `SourceDescriptor.extent`。
- `platform/windows/windows_runtime_factory.cpp` 的 `initial_overlay_extent()` 各軸套用 960／540 上限，factory 以 descriptor extent 建立 overlay。
- `platform/windows/wgc_capture_job.cpp` 的 `create_inactive()` 另以 canonical capture item 的 `Size()` 建立 allocation。
- 第一張呈現後，runtime 以 content／crop 更新 adapter aspect；使用者幾何沿既有 overlay 流程維持。
- 待驗證假說：來源列舉、最小化、還原、resize 與 Start 的時間順序，可能讓 descriptor extent 與擷取內容出現差異。此項屬定位線索。

### 重現時建議一併記錄

1. 來源程式、當時的最小化／還原狀態，以及「更新來源清單→選取→開始」的操作順序。
2. 有框／無框、Editable／PassThrough、使用中的螢幕與縮放倍率。
3. 過小的是副本外框，或正常大小外框內的擷取影像。
4. 同時期的來源 outer／client／normal rect、catalog extent、Start 時 item size、replica client／framebuffer 與第一張 presented content。

取得可重現案例後，再依實際責任位置修正來源尺寸選擇或啟動幾何，沿用現有分層與單次建構契約。
