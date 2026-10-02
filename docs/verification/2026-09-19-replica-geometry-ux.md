# 2026-09-19 Replica geometry UX 驗證

## 範圍與使用者決定

使用者要求來源表格三欄可調寬，以及 Editable 階段滾輪縮放。追加決定為預設 5%，控制面板提供 slider／numeric input，沿 `.config` 保存。初始小副本後來恢復正常，使用者將另提供重現順序，本輪以 `other-issue.md` 記錄並保留初始尺寸 production 行為。

使用者允許程式啟動與自行建立測試視窗，滑鼠鍵盤與焦點操作留給使用者。Computer Use 操作已停止。自行啟動的 Task6 snapshot controller 後續查詢已結束，相關 source capture 維持原狀。

## Spec 與 plan rolling review

- `ux_spec_review_20260919_r1`：0 Critical／0 Medium；Low 指出 SliderInt 的 text entry 為 Ctrl-click。Root 查核 pinned ImGui widget 與現有 Opacity 使用 DragInt 的差異，spec 明定 SliderInt／Ctrl-click。
- 使用者回報初始尺寸恢復正常並要求記錄，scope 改為保留初始尺寸行為。`ux_spec_review_20260919_r2` 完整 review：0 Critical／0 Medium。
- `ux_plan_review_20260919_r1`：0 Critical／1 Medium。現有 case11 的 native region tests 使用 SetCursor／SetCapture／SetForegroundWindow／SetFocus，hidden HWND 與 SendMessage 仍可帶來桌面輸入副作用。Root 逐行確認屬實。
- Plan 改成 `ctest -LE hardware -E 'case 11:'`，並對 case11 使用已核對的安全 SECTION allowlist。新增 native wheel SECTION 先查核 callback 路徑，維持 pointer gesture idle、hidden HWND 與 SWP_NOACTIVATE。
- 全新 `ux_plan_review_20260919_r2`：0 Critical／0 Medium。各階段保持新 reviewer，completed agents 維持 inactive。

## Baseline

- Repo：`D:/document/MesRepo/Airelinna`，branch `p4-application-release`，UX 前 HEAD `0fe7b1c`。
- `artifacts/ux-geometry-20260919/before/` 保存 28 個預計 touched files 的原始工作樹內容，包含既有 dirty comments／docs。
- 安全 CTest baseline：11/11，exit0，0.71 秒；log `artifacts/ux-geometry-20260919/safe-baseline-ctest.log`。
- 所有新增測試與 spec／plan／verification／issues／artifacts 維持工作樹；production 提交依三個 task 分開。

## 後續驗證入口

Task1 由 typed config／Application acceptance tests 驗證；Task2 由純值幾何與 hidden native wheel tests 驗證；Task3 由 headless ImGui events 驗證欄界與 numeric gesture。Desktop SendInput 驗收、真實 WireGuard／Slack 重現與完整效能 Task7 matrix 各自保留獨立狀態。

## Task 1 完成

- Commit：`f2b591f feat(settings): persist replica wheel zoom increment`，12 個 production files，118 insertions／3 deletions。
- RED：missing API 編譯失敗後，stub build 成功；Application 與 Config assertions 分別失敗，記錄於 `task1-red-*`。
- GREEN：Application 86 assertions、Config 145 assertions；安全 CTest 11/11，0.90 秒；source boundaries 與 15-case inventory 通過。Root 已讀回 log 核對。
- Fresh spec reviewer `ux_task1_spec_review_20260919_r1` 與 fresh quality reviewer `ux_task1_quality_review_20260919_r1` 各為 0 Critical／0 Medium／0 Low；root 核對 command、config 與 replacement 實際差異。
- Staged tree：`7ec2420a94d3e19f2500aa2f22ad30f22b18a6f1`。`task1-snapshot/source` 由 index 匯出，tests OFF／integration OFF，Release 完整 `avb0795` 與 `verify_source_boundaries` build 成功；commit 前再次核對 tree 相同。
- Tests／spec／plan／artifacts／issues 持續留在工作樹。Task2、Task3 的 prepared tests 與 stubs 維持各自階段。

## Task 2 完成

- Commit：`522c326 feat(windows): resize editable replicas with the mouse wheel`，6 個 production files，138 insertions／2 deletions。`overlay_policy.hpp` 以局部 index patch 提交，既有註解修改保留工作樹。
- Fresh spec reviewer `ux_task2_spec_review_20260919_r1`：0 Critical／0 Medium，Low 為 `SetWindowPos(FALSE)` 缺直接故障注入；root 確認成功判斷先於 remainder 更新，保留此驗證限制。
- Fresh quality reviewer `ux_task2_quality_review_20260919_r1`：1 Medium，hidden test 的固定 1050 寬度及 `width * 1.05` 預期忽略實際 work-area cap。Root 以 1366×728、4:3 上限計算與原生查詢路徑確認問題成立。
- 修正測試：pure policy 維持固定數值斷言，native fixture 改以當前 monitor／DPI／style 對照 policy 回傳的 client 矩形。新增 production abstraction 數量維持原值。
- 全新 `ux_task2_quality_review_20260919_r2` 完整複審：0 Critical／0 Medium。修正後 geometry 119 assertions、hidden native 177 assertions，另三組安全 SECTION 合計 71 assertions 通過。
- Staged tree：`a8f054b7dfd48e9ca17b558e1144042bdcd539c8`；`task2-snapshot` tests OFF 的完整 Release build／boundary 成功，commit 前核對相同 tree。

## Task 3 完成

- Commit：`d21d299 feat(ui): expose wheel zoom settings and resizable source columns`，4 個 production files 與 1 份 focused guide，192 insertions／3 deletions。
- Staged tree：`1a885e24417b9db2d15ef500e0a6a5be626cfa1e`；`task3-snapshot` 完整 Release build／boundary exit0，commit 前核對相同 tree。三個 UX commits 均排除 tests／spec／plan／verification／artifacts／issues。
- 工作樹 executable：`build/p4-release/src/avb0795.exe`，SHA256 `C9FF79F6AD4B3DA363766D9555CDF4CEB6AE943F9236CC30B841692244E3119F`。
- 精確 staged executable：`artifacts/ux-geometry-20260919/task3-snapshot/build/src/avb0795.exe`，SHA256 `C0BCC01600868382623B1D0113CE53F6293F85D7B14B2DE6AD9F2938C7F489BC`；兩份 binary 分別對應工作樹與已提交來源的建置。

- 新 UI SECTION 分別取得真實 assertion RED；GREEN 為欄界 10 assertions、wheel draft 77 assertions。Ctrl release 的 ImGui trickling 由測試明確前進一 frame 並驗證 event queue 消費，production 沿用原有 input 行為。
- Fresh spec reviewer `ux_task3_spec_review_20260919_r1` 與 fresh quality reviewer `ux_task3_quality_review_20260919_r1` 各為 0 Critical／0 Medium／0 Low，包含 scoped 新文件。
- 完整工作樹 production／core／native build exit0；root 最終重跑安全 CTest 11/11，0.73 秒，以及 geometry 119／hidden native 177／另外三組 71 assertions；15-case inventory 通過。
- Root logs：`root-final-safe-ctest.log`、`root-final-native-sections.log`、`root-final-inventory.log`。PowerShell 將 inventory 的成功 stderr 以 NativeCommandError 樣式呈現，native exit code 為 0，內容確認 15 cases。
- 使用者桌面輸入保持使用者控制。此次驗證涵蓋 headless ImGui、純值政策與 hidden native HWND；完整 case11 的 region gesture 與 hardware suite 沿本輪安全範圍排除。

## 其他工作狀態

效能 Task7 的四份文件完成獨立 review：`task7_docs_review_20260919_r1` 為 0 Critical／0 Medium，1 Low 函式名稱。Root 查核實際名稱為 `progress_backend_capacity()`，已修正文字。這些文件仍屬 Task7 工作樹；最終配對 benchmark matrix 與整體 native acceptance 維持 pending。

初始副本偶發過小保留 `other-issue.md` 待重現項目。此次 UX commit 的 factory 修改為 wheel preference 驗證與安裝，原有初始尺寸計算保持原樣。
