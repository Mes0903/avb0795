# Vulkan performance Task 2 實作驗證

日期：2026-09-16，Asia/Taipei。Production 起點：`10f0df9`；repo：`D:/document/MesRepo/Airelinna`。

## 目的與範圍

為了讓擷取與 graphics progress 保持原期限，UI 需要獨立的更新政策與完整的隱藏輸入同步。本 task 加入 std-only `UiCadence`／host wait 政策、ImGui 同步 helper、DesktopHost callback 與 event-loop 接線。ControllerView 提供本地互動狀態的值查詢，native ownership 持續歸 DesktopHost。

## Test-first 與建置

- Case 07 新增三個 SECTION：cadence、hidden／restore、真實 ImGui queue 同步。
- Case 01 新增一個 SECTION：UI command reentry 與三個 deadline 合併。
- 首次 stub RED：case 01 的 command immediate reentry 失敗；case 07 的 deadline、visibility 與 hidden 選擇失敗。
- Cadence 完成後，case 01 通過；hidden helper stub 剩四個精準失敗：backend 執行時 trickling 尚為 true、A／B 按鍵仍按住、input queue 尚有內容。
- Helper 加入範圍內 trickling override 與 RAII restoration 後，cases 01／07 均通過。
- 完整 production／四個 regression executables 建置通過；15/15 cases 通過，其中硬體 3/3，總計 4.13 秒。
- 最後補上 `HiddenSyncs`／`GraphicsOnlyWakes` counters 與 `WantTextInput` activity fact，production 重建、source boundary 與 15-case inventory 驗證通過。

## 分階段提交與 review

Foundation commit：`656bc3b`，包含兩個 policy／helper headers，151 行。完整 tree：`107509489b895d489548257302b9c340e8fd5abe`。

該階段 reviewer `task2_cadence_foundation_review_1` 為全新 agent，結果 0 Critical／0 Medium。獨立 snapshot 位於 `artifacts/vulkan-perf/task2-cadence-foundation-20260916/`，自身 first-party sources 的 Release configure、`avb0795`、`verify_source_boundaries`、diagnostics target 全部通過；`build.log` 保存證據。

完整 Task 2 reviewer：`task2_full_review_1`，0 Critical／0 Medium，已核對 callback chaining、visibility、hidden sync、所有 host loops、fatal firewall 與四組測試。後續完成實際互動 fixture、B1／B2 cadence 量測與接線 commit 的獨立 snapshot 驗證。

第二輪全新 reviewer `task2_full_review_2` 配合最小原生重現確認一個 Medium：cursor re-entry 可能重播上一幀座標。Root 獨立核對 pinned backend、GLFW `input.c` 與 `acceptance/runs/cursor-probe-01.json`。實際序列為 IO `(50,50)` → 新位置 `(100,120)` 排入 queue → Leave 保存舊 IO → Enter 重播舊 IO；GLFW 對相同位置採去重，backend 的 MouseWindow 已有效，因此最後 native 為 `(100,120)`、IO 為 `(50,50)`、queue 已清空。

這是 backend entry 邊界的相容性問題；cadence 延長事件累積窗口。最小重現使用兩個 test-owned HWND、SetCursorPos 與真實 TrackMouseEvent。純 callback 的 interleaved 對照亦保存在同份 JSON。Root 在 DesktopHost entry wrapper 轉送 backend 後加入 GLFW window-local position 取樣，使用 public ImGui queue API，整段保留原有 exception firewall。

此修正的完整 Release build、source boundaries、15/15 cases（含 hardware 3/3）與 15-case inventory 於 2026-09-16 通過；CTest 總計 3.87 秒。固定序列的 real-host RED／GREEN、完整 native acceptance 與下一輪全新 review 接續記錄。

## 實際互動與效能證據

### 數值編輯與乾淨建置的後續修正

第三輪 reviewer 找到已確認文字 62 在後續 keyboard tweak 61 結束後重新匯出的 Medium。Root 的真實 ImGui regression 重現 `61:preview 62:preview 62:commit`。Pinned widgets 的 temporary editor ownership guard 修正後，正向 active editor focus-loss 與後續 tweak 均通過。第四輪 reviewer 結果 0 Critical／0 Medium。

獨立 snapshot 進一步驗證出 Git nested-worktree prefix 會跳過依賴 patch，以及全域 `core.autocrlf` 會將輸出轉成 CRLF。修正以 archive parent 為 discovery ceiling、命令區域 `core.autocrlf=false`／`core.eol=lf` 與 patch checkout LF 組成。`task2-imgui-input-20260916-r3` 從下載的 ImGui／GLFW 建置 production、diagnostics 與 boundaries 通過；widgets SHA256 為 `d36894ba583c4a976a1f227b8721473142aa8ff669d9708173c2c2d25c339e3f`，Vulkan backend 為 `435e04fba9de28ff2c88e3cccb883c24ae387d4cea052648b0a0447921027ec0`。其 staged tree `a5452ecce335e4a86a1b7caa9769c84be9960714` 形成 dependency commit `8804a84`；原先兩個失敗 snapshot 保留證據。

第五輪 reviewer 的 dependency 審查為 0/0；完整 Task 2 審查找到另一個 Medium：numeric 48 經三次 hidden sync，ImGui 暫存文字過期。Root 加入同一測試的第三次 hidden sync，RED 顯示 `commands.size() == 0`，預期為 2。UI 對 opacity 與 region 的 DragInt 啟用 public `LiveEditOnInputScalar`，使用既有 draft 保存解析數值。Case 07 的 2／3／8 次隱藏同步、Escape 還原、region 四次同步與原有 session／rejection cases 全部 GREEN，0.27 秒；測試總 inventory 維持 15。

修正前的最新全套為 15/15、hardware 3/3，4.38 秒；`acceptance/runs/final-ui-os-01` 為 native 15/15、host/source exit 0。這些結果對應 `final-inputs/` 中的先前候選。`verified-controller-r2` 的三筆 B1 都是 598 UI builds／60 秒、complete／drain complete／exit 0；後續 UI draft 修正使用新候選重新驗收與量測。

第六輪全新 reviewer `task2_full_review_6` 先核對 Task 2 與原 opacity spec，再審查 code quality，結果 0 Critical／0 Medium；rolling review 於此收斂。最後 production build、source boundary、15-case inventory 及全套 15/15（hardware 3/3）通過，CTest 4.09 秒。`task2-host-final-20260916` 的 staged tree 為 `a6dfd73068f7946ab0c1fd31efe2731bd0c28cae`，獨立 source snapshot 的 executable／diagnostics／boundary targets 均建置成功。

最終 native acceptance 使用上述 snapshot headers 與 `final-archives`，將後續 Task 3 工作樹隔離於驗收輸入之外。`final-pressure-os-01` 通過 15/15、host/source exit 0、stderr 0 bytes。三個 controller submission records 的成功 fence proof 暫存於驅動：容量 3 全滿時，UI frame 288→292、auto-refresh 命令被接受，而 submissions 保持 288；解除後 submissions 291、held records 0，早於 3 秒 timeout。這個實驗觀測受控 completion-proof delay 下的 admission 與 UI 行為，硬體本身照常執行。

同次驗收向自有 HWND 傳送 synthetic `WM_DPICHANGED`：144 DPI 對應一次 scale 1.5 callback，suggested／actual rectangle 相符，client／DisplaySize／framebuffer 為 688×892；實際 monitor DPI 保持 96。物理跨螢幕 DPI migration 與 HID 另列驗證範圍。`acceptance/final-pressure-inputs/manifest.json` 保存九個精確 SHA256，包含 fixture source、CMake、executables 與輸出；root 另核對完整 JSON 的 assertions。

Native acceptance fixture：`artifacts/vulkan-perf/task2-20260916/acceptance/`。這份本機驅動沿用受控來源與 production archives，使用實際 DesktopHost loop；source/start setup、native input、shipping executable 與 physical HID 的證據邊界分別記錄。

Task 1 baseline 保留原 source、binary、reports 與 fingerprints。候選使用新的 build／output 目錄。正式 B1／B2 每個情境三次、10 秒 warmup 與 60 秒 measurement，量測期間保持建置與硬體測試暫停。

最終 wiring commit：`01c32d66d1edd8f1ba7a5fcba993e0be4f5aa2a3`。正式 B1 三次 UI builds 為 598／622／624；B2 為 599／598／598，CPU 為 2.3429%／2.1853%／2.1090%。B2 CPU median 2.1853%，相較 Task 1 的 12.1005% 減少約 81.94%；UI build p95 median 從 0.1285 ms 增為 0.3420 ms。建構頻率、單次成本與程序成本一併解讀。六筆候選皆 complete、drain complete、exit 0、stderr 0；root 重算十二份 baseline／candidate report SHA256，與 final-comparison.json 全部相符。每次 run 的 manifest、來源與觀測範圍見 [Task 2 報告](../../artifacts/vulkan-perf/task2-20260916/README.md)。Task 7 保留最終交錯 A/B 驗收。

Tests、spec、plan、verification 與 artifacts 保留於工作樹。
