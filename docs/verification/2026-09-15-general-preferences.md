# 通用偏好保存驗證紀錄

## 範圍與基準

- 日期：2026-09-15。
- Repo：`D:/document/MesRepo/Airelinna`；branch：`p4-application-release`。
- 使用者確認八類通用偏好；視窗配置、來源、選區與草稿維持當次執行狀態。
- 16 個可能修改檔案的 prefeature baseline：`D:/document/MesRepo/5568ke/build/avb-general-preferences-baseline-20260915`。
- 修改前 CTest：12/12 非硬體案例通過。
- 既有 rename／UX／opacity／crop 內容保持原樣，index 初始為空。

## Spec 與 plan review

- Spec round 1：全新 reviewer，0 Critical / 0 Medium。
- Plan round 1：全新 reviewer，0 Critical / 0 Medium；修正新檔缺席 fallback 的文字。
- 正式規格：[通用偏好保存](../superpowers/specs/2026-09-15-general-preferences-design.md)。
- 計畫：[General Preferences](../superpowers/plans/2026-09-15-general-preferences.md)。

## 實作驗證追蹤

- [x] Case 04 新欄位與 legacy fallback 紅綠驗證。
- [x] Case 01 接受／保存、重新初始化、失敗重試與暫時值邊界。
- [x] Release build、非硬體 CTest、source boundaries、inventory。
- [x] 實作與文件完整 rolling review。
- [x] Index 與差異檢查。

## 已取得的執行證據

- RED：成功 SetMode 的保存次數預期 1、實際 0；新 optional keys 使既有 FPS 144 回到預設 60。
- RED：legacy fallback、重新初始化的模式／checkbox、自動刷新、非法 mode admission 與九個 root keys 均觀察到缺少行為。
- 測試修正：重新啟動使用新 queue／服務生命週期；移除 Windows 非法檔名必定回報 exists 錯誤的假設。這兩個 fixture 問題與產品保存缺口分開處理。
- GREEN：case 01／04 共 1,631 assertions、2 cases 通過。
- 主 agent 重新建置 `avb0795`、`avb0795_core_tests`、`avb0795_windows_policy_tests`：exit 0。
- 主 agent 完整非硬體 CTest：12/12、0 failures，執行時間約 0.77 秒。
- `VerifySourceBoundaries.cmake`：exit 0。
- `VerifyTestInventory.cmake`：exit 0；維持 15 個 V1 cases，hardware 13–15。
- 本次新增 6 個 SECTION，其餘利用既有 scheduler／selection／settings assertions。保存次數變更分別對應 mode 新增寫入、mode/FPS/border 三次接受、mode 加 opacity flush。
- 本地文件連結檢查通過。

## 實作 review

Task 1 spec-compliance：全新 reviewer 逐項核對 baseline 與 live code，0 Critical / 0 Medium。

完整品質與文件 round 1：另一位全新 reviewer 獨立對照 10 個程式／測試檔與 7 份文件，0 Critical / 0 Medium。審查涵蓋新舊路徑優先序、初始化生命週期、mode admission、全域快捷鍵、框選暫時值、capability fallback、自動刷新期限及保存失敗重試。六個新增 SECTION 與文件契約一致。

16 個基準檔案的差異格式檢查與 Git diff check 通過；index 維持空白。程式、文件與驗證修改保留工作樹，提交整理方式等待使用者選擇。

執行檔：`build/p4-release/src/avb0795.exe`。設定檔目的地為執行檔旁的 `avb0795.config`，新檔缺席時讀取同目錄 `airelinna.json`，成功保存使用新路徑並保留舊檔。

## 人工驗收

自動化結果與人工操作各自記錄。以下由實際操作者確認：

- [ ] 修改語言、快捷鍵、兩個 checkbox、FPS、邊框、模式與不透明度，完成操作後關閉並重新啟動。
- [ ] 確認上述偏好恢復，來源仍由當次操作選擇。
- [ ] 使用既有舊設定檔啟動，修改設定後確認新檔產生且舊檔維持原內容。

Tests、spec、plan 保留工作樹。此次通用偏好變更的驗證以 portable／policy 路徑為主，先前 graphics hardware 執行結果見 [透明度與選區驗證](2026-09-15-opacity-region.md)。
