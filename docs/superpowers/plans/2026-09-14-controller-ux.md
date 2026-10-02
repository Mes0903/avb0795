# 控制面板 UX Implementation Plan

> For agentic workers: use subagent-driven-development for the independent native repair and executing-plans for the coupled application/UI changes. Review each completed stage against the spec, then inspect code quality with fresh reviewers.

Goal：完成來源清單配色、HWND 顯示、5 秒自動更新、快捷鍵錄製與模式／邊框修正

完成狀態：2026-09-14 已完成實作，完整測試 12/12 成功，最後一輪全新 reviewer 回報 0 Critical／Medium。 詳見[驗證紀錄](../../verification/controller-ux-2026-09-14.md)

Architecture：UI 管理來源列與局部錄製狀態，Application 排程來源更新。 Windows 層提供診斷字串和已驗證的快捷鍵值，host 根據即時焦點路由錄製輸入。 adapter 管理輸入樣式，overlay 管理邊框

Tech Stack：C++23、Dear ImGui 1.92.9、GLFW、Win32、Catch2、CMake 與 Lua 文件檢查

## 工作樹與驗證命令

沿用 `D:/document/MesRepo/Airelinna` 的 `p4-application-release` 工作樹。 原有程式修改保持原狀，相關檔案備份位於 `D:/document/MesRepo/5568ke/build/airelinna-ux-baseline-20260914`。 本次產物留在工作樹，測試與規劃記錄保留為本機檔案

在 Visual Studio 2026 x64 開發者命令環境執行：

```text
cmake --build build/p4-release --target Airelinna airelinna_core_tests airelinna_windows_policy_tests verify_source_boundaries
ctest --test-dir build/p4-release --output-on-failure -LE hardware
```

預期完整測試結果為 12/12 通過。 單項使用現有 Catch2 標籤與 SECTION 名稱，例如：

```text
build/p4-release/tests/airelinna_core_tests.exe "*case 01*"
build/p4-release/tests/airelinna_core_tests.exe "*case 07*"
build/p4-release/tests/airelinna_windows_policy_tests.exe "*case 10*"
build/p4-release/tests/airelinna_windows_policy_tests.exe "*case 11*"
```

## Task 1：模式與邊框的原生交易

檔案：platform/windows/detail/overlay_transactions.hpp、platform/windows/win32_overlay_adapter.cpp、platform/windows/win32_overlay_adapter.hpp 與 tests/windows/overlay_policy_test.cpp

- [x] 在 case 11 增加一個隱藏視窗 SECTION，以實際 Win32 adapter 執行雙向切換。 視窗使用目前行程建立的測試資源，RAII 在結束時卸載 adapter 與銷毀視窗
- [x] 回讀 GWL_STYLE、GWL_EXSTYLE 與 adapter.mode，確認舊版在切換邊框後退出穿透的失敗步驟。 將觀察列入測試失敗訊息
- [x] 依原生證據修正 leave_pass_through，將長期恢復目標限定於 transparent/layered 輸入樣式，保留目前其餘 EXSTYLE 位元。 完整快照仍負責一次失敗交易的回復

核心目標值為：

```cpp
constexpr auto owned_bits = transparent_style_bit | layered_style_bit;
NativeWindowState target{
  .extended_style = (current->extended_style & ~owned_bits) |
                    (snapshot.extended_style & owned_bits),
  .layered_attributes = snapshot.layered_attributes,
};
```

- [x] 對 target 執行既有 apply/read-back，失敗時回復 current 並驗證結果。 adapter 的 Doxygen 清楚區分長期輸入狀態與短期交易快照
- [x] 增加一個既有 fake operations SECTION，驗證期間新增和移除的非輸入位元、原有 layered attributes 與失敗回復。 原生測試通過後執行完整 case 11
- [x] 以全新 reviewer 檢查需求符合度，再檢查交易與生命週期責任，處理確實成立的 Critical/Medium

## Task 2：來源清單與 Application 更新期限

檔案：include/airelinna/core/types.hpp、include/airelinna/app/commands.hpp、include/airelinna/app/controller_view_state.hpp、src/app/controller_view_state.cpp、include/airelinna/app/application.hpp、src/app/application.cpp、platform/windows/windows_source_catalog.cpp、src/ui/controller_view.cpp、include/airelinna/i18n/catalog.hpp、src/i18n/catalog.cpp、tests/app/application_contract_test.cpp、tests/app/controller_contract_test.cpp

- [x] 先在 case 07 使用真實 ImGui 畫面檢查來源表格、類型配色、長名稱字面文字與移除底部 Quit，取得 RED
- [x] SourceDescriptor 增加擁有字串的 native_handle_text，ControllerSourceRow 增加相同字串與 SourceKind。 Windows 在 enumerate 中建立 `std::format("0x{:X}", reinterpret_cast<std::uintptr_t>(window))`，顯示器使用空字串
- [x] UI 使用三欄 ImGui table、固定類型欄／HWND 欄與伸展名稱欄，整列 Selectable 的 ID 以 SourceId 建立。 類型顏色為粉紅色／藍紫色，HWND 綠色，名稱沿用 Text。 停用列的文字使用 TextDisabled 並保留選取底色。 在停用 scope 外提供長名稱 tooltip
- [x] 使用單一固定列高，省略每列 disabled_explanation，移除底部 Quit。 新增 TextId 提供 English、繁體中文與既有 Japanese 對應字串
- [x] 新增值命令與期限欄位：

```cpp
struct SetAutoRefresh { bool enabled{}; };
bool auto_refresh_enabled_{};
std::optional<Clock::time_point> source_refresh_deadline_;
```

- [x] case 01 先測試新命令排程。 以可控制的 now 驗證 4999 ms 保持列舉次數、5000 ms 增加一次、延遲 30 秒仍增加一次、手動嘗試重排與列舉失敗保留原快照。 取得 RED 後實作
- [x] Application 在命令處理後依閒置、runtime、stop、quit 和 selected_source 判斷可更新條件。 可更新時，以空期限安排 now + 5s，到期執行 refresh_sources 並安排 now + 5s。 暫停時清空期限
- [x] 手動 refresh handler 使用 now，成功或失敗都重排期限。 勾選指令設定偏好並安排完整間隔，取消指令與 shutdown 清空期限。 next_deadline 對 runtime、immediate 與 source 三個 optional 取最早時間
- [x] ControllerViewState 增加 auto_refresh_enabled 與 auto_refresh_paused。 checkbox 的 admission 依 Application 結束意圖決定，暫停狀態顯示於 checkbox 旁
- [x] 驗證選取優先於同輪到期刷新、runtime/drain 維持暫停、關閉清空期限、連續刷新保留先前 notice 規則。 執行 case 01 與 07，並安排全新 review

## Task 3：快捷鍵錄製與全域事件路由

檔案：include/airelinna/ui/controller_view.hpp、src/ui/controller_view.cpp、include/airelinna/desktop/desktop_host.hpp、src/desktop/desktop_host.cpp、platform/windows/windows_global_hotkey.hpp、platform/windows/windows_global_hotkey.cpp、main.cpp、include/airelinna/i18n/catalog.hpp、src/i18n/catalog.cpp、tests/app/controller_contract_test.cpp、tests/windows/windows_policy_test.cpp

- [x] case 07 建立真實 ImGui 錄製測試：點擊錄製、釋放起始輸入、送 Ctrl + K、確認後取得一筆 ReplaceHotkey。 另測 Esc、失焦、session 更換與停用時取消。 先取得 RED
- [x] ControllerView 以一個小型局部狀態結構保存 session、候選與等待釋放旗標，替換 hotkey_edit_text_。 modal 置於 root render 路徑，讓來源內容的捲動與裁切保持獨立
- [x] ImGui key 映射沿用 A-Z、0-9 與 F1-F12 的連續列舉範圍，修飾鍵使用 io.KeyCtrl、KeyAlt、KeyShift、KeySuper。 主鍵用 IsKeyPressed(key, false)，Esc 優先取消，多個主鍵同時按下時保持等待並顯示提示
- [x] 錄製按鈕保留明確的穩定 ID，候選顯示目前輸入與確認／取消按鈕。 開啟 modal 時要求起始按鍵與滑鼠釋放，確認提交後清除局部狀態。 視窗失焦檢查在每輪 root render 開頭執行
- [x] WindowsGlobalHotkey 的 sink 改為 `std::function<void(core::HotkeyChord)>`，既有 payload 驗證成功後傳送已確認的 chord。 case 10 沿用既有 stale、reentrancy 與例外測試，將輸出斷言改為 typed chord
- [x] 增加同步 host 輸入入口與 UI 值接收入口：

```cpp
[[nodiscard]] bool DesktopHost::try_record_hotkey(core::HotkeyChord chord) noexcept;
[[nodiscard]] bool ControllerView::try_record_hotkey(core::HotkeyChord chord) noexcept;
```

host 在原生控制面板具有焦點且生命週期允許時呼叫 UI 入口，recorder 存在時消費事件並更新候選。 等待起始鍵釋放時消費事件並保持等待。 host 失焦時取消局部錄製並回傳 false。 main 路由內容：

```cpp
[&commands, &host](core::HotkeyChord chord) {
  if (host.try_record_hotkey(chord)) {
    host.wake();
  } else if (commands.push(app::RestoreActiveReplicaEditable{})) {
    host.wake();
  }
}
```

- [x] 真實 ImGui SECTION 注入同一個已確認 chord，驗證候選更新和零 Restore 輸出，取消後接收入口回傳 false。 檢查 host 的即時 GLFW_FOCUSED 路由及 shutdown/fatal gate
- [x] 保留 ReplaceHotkey 的 Application 註冊／保存契約，執行 case 01、07 與 10。 全新 reviewer 先比對規格，再檢查分層、焦點時序與重複事件

## Task 4：文件與整體驗收

檔案：docs/architecture/application-ui.md、docs/architecture/windows-runtime-overlay.md、docs/architecture/desktop-host.md、docs/architecture/windows-capture.md、docs/architecture/README.md

- [x] 更新實際受影響的來源欄位、timer、recorder、全域鍵路由與樣式恢復段落，修正舊的文字輸入說明。 維持中文目的、需求、資料結構的敘述順序與 Mermaid 分層圖
- [x] 執行 native Release 全部目標、12 個非硬體案例、verify_source_boundaries 與 git diff --check
- [x] 使用 `C:/lua/lua.exe -E C:/Users/Mes/.codex/skills/zhTW-translocalizer-skills/scripts/lint.lua --profile final --format markdown` 檢查所有本輪變更的 Markdown，逐筆核對警告
- [x] 全新 reviewer 完整檢查這次 diff 與相關責任範圍，逐項驗證 Critical/Medium 的實際證據，修正後再以全新 reviewer 複查
- [x] 回報執行檔位置、真實驗證結果、自動更新暫停規則與人工外觀檢查範圍
