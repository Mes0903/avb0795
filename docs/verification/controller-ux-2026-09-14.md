# 控制面板 UX 驗證紀錄

## 範圍與結果

為了讓來源選取、互動模式及快捷鍵設定具備一致的操作回饋，本輪完成六項 UX 調整。 來源類型分別採粉紅色與藍紫色，名稱使用一般文字色，視窗 HWND 使用綠色。 來源列改為三欄並以灰色呈現停用狀態，固定標題列保留關閉入口

自動重新整理預設關閉，勾選後每 5 秒更新一次。 已選來源、擷取及清理期間呈現暫停狀態。 快捷鍵使用錄製面板收集候選，再由確認按鈕提交。 模式與邊框各自保存已接受的設定

## 建置與回歸證據

執行環境為 Windows、Visual Studio 2026 x64 developer environment、clang-cl、NMake 與 native Release。 執行檔為 `build/p4-release/src/Airelinna.exe`

```text
cmake --build build/p4-release --target Airelinna airelinna_core_tests airelinna_windows_policy_tests verify_source_boundaries
ctest --test-dir build/p4-release --output-on-failure -LE hardware
git -c core.safecrlf=false diff --check
```

結果：產品與測試建置成功，分層檢查成功，完整測試 **12/12 通過**，耗時 0.70 秒。 Git 差異空白檢查成功

| 聚焦檢查 | 實作前證據 | 最終驗證 |
| --- | --- | --- |
| 來源表格 | `table == nullptr` | 19 個 assertions：分欄、配色、HWND、字面文字、命令與停用狀態 |
| 自動更新 | 14 個排程斷言失敗 | 36 個 assertions：期限、手動重排、失敗保留、選取及 runtime 暫停 |
| 錄製面板 | 開啟後 popup 數量為 0 | 68 個 assertions：三段主鍵、確認、取消、失焦與多鍵等待 |
| 原生快捷鍵 | typed chord 數量為 0 | 完整 case 10：300 個 assertions |
| 模式與邊框 | 雙向操作共 8 個原生斷言失敗，模式維持穿透 | 完整 case 11：545 個 assertions |

模式問題的實測關鍵是 `WS_EX_WINDOWEDGE` 的 `0x100` 位元隨邊框改變。 退出穿透模式改為恢復輸入相關位元與原有 layered attributes，完整 current snapshot 持續負責失敗回復

來源表格的 `"HWND"` 標題已納入 UI 文字的精確掃描例外。 小型 probe 確認字面標題成功通過，裸 `HWND handle` 宣告仍觸發原生型態邊界錯誤

## 獨立 review

每輪 review 由新的 reviewer 執行，主 agent 逐項核對實際程式路徑與證據

| 階段 | 結果與處理 |
| --- | --- |
| Spec 第 1 輪 | 1 Medium：補入錄製既有全域快捷鍵的路由契約 |
| Spec 第 2 輪 | 0 Critical／Medium |
| Plan 第 1 輪 | 0 Critical／Medium |
| 原生模式實作 | 0 Critical／Medium |
| 完整實作第 1 輪 | 1 Medium：同輪失焦與回焦合併，改由 host 失焦 callback 立即取消 |
| 完整實作第 2 輪 | 0 Critical／Medium，結束 review |

## 文件與人工操作範圍

架構文件同步涵蓋來源診斷值、自動更新期限、錄製狀態、typed chord 路由及模式位元的責任。 使用下列命令檢查本輪 Markdown：

```text
C:/lua/lua.exe -E C:/Users/Mes/.codex/skills/zhTW-translocalizer-skills/scripts/lint.lua --profile final --format markdown <本輪文件>
```

剩餘提示經逐項核對，對應既有列舉、技術檔案清單，以及「通過檢查」的 pass 語意。 新增段落的斷句、空白與繁體字已修整

自動驗證使用真實 ImGui frame 和隱藏的原生測試視窗。 實際桌面外觀、錄製時切換前景應用程式，以及 WGC 擷取中的操作感受，交由使用者開啟更新後執行檔確認

Git branch 維持 `p4-application-release`，staged 集合為空，原有工作樹內容與本輪程式、文件、測試及規劃記錄一併保留於本機
