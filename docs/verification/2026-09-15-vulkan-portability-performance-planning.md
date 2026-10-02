# Vulkan portability／performance 文件驗證紀錄

建立日期：2026-09-15。完成日期：2026-09-16，Asia/Taipei。範圍：spec → rolling review → plan → rolling review。

## 工作基線

- 專案：`D:/document/MesRepo/Airelinna`，產品 avb0795，namespace `avb::`。
- HEAD：`a955d2e`。現有工作樹包含 rename、UI、opacity／crop、preferences 與文件變更；本輪來源判讀使用這份工作樹。
- 本輪開始時記錄 130 個 source／build／test／tool／resource 檔案 SHA256，作為交付範圍檢查。
- 本輪交付為本紀錄、spec 與 plan；production sources 維持原有內容，staging 保持原樣。
- 現有 build／hardware 結果作為歷史背景。本輪驗證聚焦文件、來源對照與 review。

## Spec review

Spec：[共用 Vulkan 邊界與效能規格](../superpowers/specs/2026-09-15-vulkan-portability-performance-design.md)。

### Root self-review

確認 scope、source lease／Vulkan image borrow／present record 三種生命週期，並核對目前 facade 的公開 API、D3 Signal failure、同 generation fallback 與 runtime generation 管理。查核後為 diagnostics 增加明確 scope、bridge preference 與穩定 session/generation 的量測窗口；provenance 由同 run ID 的外部 manifest 記錄。

### Round 1

- Reviewer：`/root/vulkan_spec_review_1`，全新 context、read-only。
- 結果：0 Critical／1 Medium。
- 審查版本 SHA256：`934610C8AED1B7E89FDDBB72FE770C0E310B95FFEDA01AD111936D14185325ED`。
- M1：Bootstrap 將實際 NT handle probes 排在 authoritative allocation 前。Root 直接核對 `windows_runtime_factory.cpp` 的 inactive WGC／initial allocation／rebuild 順序，以及 render stack 的 `collect_bridge_capabilities`、`query_interop_slot`；確認為真實問題。
- 修正：device attach／procedure resolve／capability prefix → WGC initial allocation → 對真實 ring textures 逐 handle probe → registration。另將 throughput 與 latency 警戒寫成兩個獨立條件。

### Round 2

- Reviewer：`/root/vulkan_spec_review_2`，全新 context、read-only。
- 結果：0 Critical／1 Medium。
- M1：Hidden 單次 input frame 與 ImGui trickle queue 的 backlog 衝突。Root 直接核對 pinned ImGui `UpdateInputEvents` 的 key-change break、queue erase 與 focus clear，確認同批 down/up/focus 可跨幀殘留。
- 修正：hidden synchronization 暫停 trickling、執行 NewFrame／EndFrame 後還原；可見 UI 保留原設定。將同批 release／focus 與後續閒置情境納入既有 cadence SECTION。

### Round 3

- Reviewer：`/root/vulkan_spec_review_3`，全新 context、read-only。
- 結果：0 Critical／0 Medium，完整 spec gate 通過。
- Reviewer 核對建構／實際 handle probes、slot handshake／雙 API proof、ImGui hidden queue drain、WSI pools、CPU padding、target 邊界及量測／測試範圍。
- Gate 通過後開始撰寫 plan。Spec 的通過狀態欄更新為本輪結果。

## Plan review

開始條件已達成：spec rolling review 第三輪為 0 Critical／0 Medium。

Plan：[七階段實作計畫](../superpowers/plans/2026-09-15-vulkan-portability-performance.md)。

### Root self-review

逐節對照 spec coverage、14 組 SECTION 預算、實際 CMake targets／commands、Windows facade 方法與 desktop progress 方法。補上 hidden input helper 的檔案責任，並將 Task 1 的時間戳欄位與 Task 3 的 FrameStamp 收斂明確分階段。

### Round 1

- Reviewer：`/root/vulkan_plan_review_1`，全新 context、read-only。
- 結果：0 Critical／4 Medium。
- M1：Detach API 的 identity。Root 對照 plan 的無參數 signature 與 spec stale-key 契約，確認缺口；改為 `detach_ring(expected_ring_serial)`，mismatch 保留當前 ring。
- M2：Host command-before-wait 測試接點。Root 核對 `desktop_host.cpp::run` 與 core tests link list，確認 case 01 原本涵蓋 application、host wait 決策位於另外的 owner；新增 production-used `choose_host_wait` 純政策，保留同一組 SECTION。
- M3：Partial rollback 的 production 接線。Root 讀取現有 constructor catch cleanup 與 `FactoryOverlayAttachmentGuard<Operations>`；新增相同模式的 `GraphicsConstructionGuard`，由真實 bootstrap／ring transaction 與測試 ledger 共用，區分 aggregate 與 ring cleanup scope。
- M4：可建置 commit 的證據。Root 查核 HEAD 的 `airelinna_*` targets 與目前 untracked `include/avb`／localization／tests，確認 dirty build 可掩蓋 staged 相依缺口；加入 V7 index snapshot production-only configure/build，前置 rename/source hunks 依既定授權流程整理。

### Round 2

- Reviewer：`/root/vulkan_plan_review_2`，全新 context、read-only。
- 結果：0 Critical／0 Medium，完整 plan gate 通過。
- Reviewed plan SHA256：`9975FA54F0250495C5AE4A8DCA62BD20E9C9DA50A3AC7E485265FB1AA10D07C6`。
- Reviewer 全文核對 spec coverage、constructor／ring guard scope、keyed detach、host wait 政策、native proof／recovery、diagnostics／benchmark、測試接線與 V7 snapshot gate。
- Gate 通過後更新 plan 狀態欄與自檢勾選。Task 1–7 維持待執行狀態。

## 交付檢查

Spec 共三輪，plan 共兩輪，每輪皆為全新 reviewer。共確認並修正 6 個 Medium，最終兩道 gate 均為 0 Critical／0 Medium。各 reviewer 已完成／轉為 inactive，後續 review 使用新的 agent identity。

本輪範圍為三份規劃／驗證文件；程式行為與既有工作樹內容維持原樣。Build／tests／hardware／benchmarks 的命令與驗收值屬後續實作計畫。本輪交付檢查包含：文件 links、Markdown fences、文字檢查、130 個 source/build/test/tool/resource hashes，以及 Git staging。

| 檢查 | 結果 |
|---|---|
| 130 個來源基線 hashes | 全部一致，變更數 0 |
| 三份文件 local links | 全部可解析至現有檔案 |
| Markdown fences／行尾 | 成對，trailing whitespace 0，檔尾 newline 完整 |
| Placeholder／指定措辭檢查 | 命中數 0 |
| Staged changes | 空；三份文件均為 untracked |
| HEAD | `a955d2e`，維持原值 |
| Spec 最終 SHA256 | `CC4D4C65C28C09C988574DEC21DACD57C8FD9FE26B2DD6D327587B2B834557A9` |
| Plan 最終 SHA256 | `2C92E0F1B223CBE0F556ADC83AF8713C29D2E4CB304C9EA21094BAE1240FAFC9` |
