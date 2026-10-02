# 架構教材驗證紀錄

本紀錄彙整 spec、plan 與教材實作的來源核對及驗證證據

## 起始範圍

- 起始 inventory：168 個檔案
- HEAD：`a955d2e110474ee339b5c3056a303eaf472b746d`
- Index SHA256：`7654B1B20FFDD15D19FFC43DBC26030C7EE2BF9C2684F50DE5D8604BAD79FE82`
- 雜湊快照：`build/architecture-handbook-validation/baseline.json`，於 spec 建立後涵蓋 169 個檔案
- `imgui.ini` 登記檔名、長度與修改時間，內容保持私有

## Spec review

### 第一輪

Reviewer：`handbook_spec_review_r1`，fresh context

| 等級 | Finding | 主 agent 核對與處理 |
| --- | --- | --- |
| Medium | 保活元件被描述為持有 DWM 資源 | `browser_capture_keep_alive.hpp` 的 `class_atom_`、`window_`，以及 `.cpp` 的 RegisterClassExW／CreateWindowExW／DestroyWindow／UnregisterClassW 確認 owner 是 class ATOM 與 helper HWND，spec 已修正 |
| Low | Lua 診斷被描述為排序 | `test_inventory.lua` 依 `ipairs(tests)`，再依 `ipairs(profile.ids)` 累積訊息，spec 已改為依輸入再依 profile 順序累積 |

### 第二輪

Reviewer：`handbook_spec_review_r2`，fresh context。 全份規格與 composition、Application／runtime、helper、CMake、shader／sampler／blend、language 與 Lua 的來源核對完成。 結論：critical 0、high 0、medium 0 與 low 0，進入 plan

## Plan review

### 第一輪

Reviewer：`handbook_plan_review_r1`，fresh context。 結論：critical 0、high 0、medium 1 與 low 0

Finding 是 Mermaid 幾何可讀性需要可執行的視覺驗證步驟。 主 agent 對照 spec 五組驗證中的圖形要求，以及原 plan 的 parse／語意檢查，確認需要補充實際圖像。 Task 3 已加入 headless browser 渲染全部圖、逐頁圖像檢視與密集圖完整尺寸檢視，將 SVG、PNG 與閱讀結果分別記錄

### 第二輪

Reviewer：`handbook_plan_review_r2`，fresh context。 結論：critical 0、high 0、medium 0 與 low 0。 Source scopes、五組驗證及完整圖像檢視流程已完成審查，進入實作

## 文件實作驗證

狀態：11 頁教材完成。 Spec、plan、文件符合度與完整來源品質各完成 rolling review，最後一輪的 critical、high、medium 及 low 都是 0

### 視覺預檢

`render-docs.mjs` 使用本機 Chrome 獨立 profile、loopback server 與原始 Mermaid bundle，以 strict browser DOM 產生 SVG、原始尺寸 PNG 與各頁 contact sheet。 2026-09-09 的三頁初稿預檢產生 17 張圖，結果 exit 0，資料位於 `build/architecture-handbook-validation/visual-1788914478423-52876/report.json`

主 agent 檢視 README、build 與 Lua 三張 contact sheets，將總覽的輸入／狀態顯示分成階段節點，將過長的 build／shader／profile 路徑改為直向，並讓 target 圖的箭頭圖例集中於正文，以增加閱讀空間。 接續以新圖像核對改動

### 實作符合度 review

第一輪 reviewer：`handbook_implementation_spec_r1`。 發現 medium 1、low 2，主 agent 完成以下來源核對及修正：

| 等級 | 核對來源 | 修正 |
| --- | --- | --- |
| Medium | `windows_runtime_factory.cpp` 建立 stack、inactive job、initial allocation 及 slots 的順序 | Renderer 開頭依實際建構順序敘述，說明 slots 取用 pool 實際 allocation 的原因 |
| Low | `frame_intake_gate.hpp` 的 busy／pending CAS 與 release。 `wgc_capture_state.hpp` 的 epoch policy | 逐檔索引將 intake gate 的責任收斂為接收權互斥、pending 合併與釋放 |
| Low | `windows_source_catalog.cpp` 的 visit 返回與外層例外邊界 | 改寫單一候選資訊查詢及 WGC 建立失敗後接續列舉的句子 |

第二輪 reviewer：`handbook_implementation_spec_r2`，fresh context。 完整重讀 design、plan 與 11 頁教材。 本輪提出 low 1：Lua 字串操作的大小單位。 主 agent 對照 `io.read(limit + 1)`、`#text` 及 `sub(1, 300)`，將文字改為位元組。 Reviewer 再以 Lua 的三位元組中文字串覆核並確認修正。 最後結論：critical 0、high 0、medium 0 與 low 0

主 agent 另對照 `main.cpp` 與 `desktop_host.cpp`，將 runtime drain 段落的 host owner 寫明為程序組裝入口，並把 Application 頁的 host 呼叫圖分成 `step → Application` 與 `render → ControllerView`

### 完整來源品質 review

第一輪 reviewer：`handbook_source_quality_r1`，fresh context。 完整核對 11 頁與各層來源後，初始 finding 為 critical 0、high 0、medium 1 與 low 0。 Medium 是 controller recovery 的完成條件

主 agent 對照 `desktop_host.cpp` 的 present 建立、結果分類與 `progress_presents()`，以及 `graphics_retry_policy.hpp`，確認 controller 由 `recovery_candidate` 產生 `PresentRecord::confirms_recovery`

該標記、成功退休的 present fence 與存在的 episode 共同構成重設條件。 `OUT_OF_DATE` 及 `SURFACE_LOST` 的 `Enqueued` 紀錄也保留標記。 Desktop 教材已依這組實際條件改寫，產品原始程式碼保持起始內容

第一輪 reviewer 重新核對修正，各等級剩餘 finding 歸零。 主 agent 結束其執行後，建立 fresh reviewer `handbook_source_quality_r2`，重新審查全份教材與來源

第二輪 reviewer 完整讀完 spec、plan、11 頁教材與 173 列索引，對照 Core、Application／UI、i18n、factory／runtime、WGC、renderer、desktop、helper、overlay／native adapters、CMake 與 Lua。 結論為 critical 0、high 0、medium 0 及 low 0

本輪獨立核對 controller 修正、callback admission／pending／wake、registry lease、CPU／GPU copy 與 fallback、submission／present ownership、WSI recovery，以及各原生轉接器的交易與清理。 Reviewer 執行 checker 得到 exit 0。 82 圖、633 個連結、173 個檔案對應 173 列索引及 158 個受保護檔案均相符

各輪 review 使用全新的 agent。 完成後結束 reviewer 執行，最後一輪也已完成這項席次清理。 本次來源審查採靜態閱讀及文件檢查，產品、GPU、硬體與建置的執行驗證由各自流程承接

### Mermaid 語法、圖像與閱讀尺度

工具版本為 Lua 5.5.1 與 Node v22.16.0。 Parser smoke 同時接受合法 flowchart／sequence，並拒絕兩種刻意截斷的輸入。 全套 checker 的一次性實作與產物保存在忽略的 `build/architecture-handbook-validation/`

| 頁面 | Mermaid 數量 | 圖像檢視 |
| --- | --- | --- |
| README | 5 | 全頁 gallery、ownership 與主執行緒 sequence 原始尺寸 |
| Application／UI | 8 | 全頁 gallery、主圖原始尺寸，設定與 widget 路線改為直向 |
| Core | 7 | 全頁 gallery、session 狀態圖原始尺寸，hotkey 路線改為直向 |
| Localization | 6 | 全頁 gallery，catalog 與 selector 路線改為直向 |
| Windows capture | 8 | 全頁 gallery、來源 publication sequence 原始尺寸，主圖與 checkout 路線改為直向 |
| Windows runtime／overlay | 15 | 全頁 gallery、主圖與 factory sequence 原始尺寸，交易與 system adapters 路線改為直向 |
| Browser keep-alive | 5 | 全頁 gallery，資格、owner 與 grace 路線 |
| Renderer | 9 | 全頁 gallery、owner／interop／texture mapping 原始尺寸，CPU／selection／viewport 路線改為直向 |
| Desktop host | 7 | 全頁 gallery、組裝主圖原始尺寸，controller 建立路線改為直向 |
| Build／validation | 6 | 全頁 gallery，target、dependency 與 shader 路線 |
| Lua tooling | 6 | 全頁 gallery，launcher、decoder、profile 與 validator 路線 |

完整 82 圖真實 browser 渲染 exit 0，產物為 `visual-1788915049238-48608/report.json`。 原始 Mermaid bundle 在 strict DOM 下產生 SVG 與 PNG。 主 agent 已檢視 11 頁 galleries，接著將 19 個過寬的橫向 flowcharts 改為直向

更新的七頁共 60 圖再次渲染 exit 0，產物為 `visual-1788915330567-68324/report.json`，並完成七頁新 galleries 的檢視。 再次調整 Application 的呼叫目標與 runtime 主圖的標籤布局後，兩頁 23 圖重繪 exit 0，產物為 `visual-1788915515806-58992/report.json`。 兩張更新主圖已以原始尺寸檢視。 各次 run 具有獨立 profile，結束後 browser 與 loopback server 隨 helper 收束

三次 report 中具有文字的標籤都具正值尺寸，SVG content bounds 都涵蓋於 viewBox。 語法解析、實際渲染與人工閱讀分別保留證據

完稿時依頁面與圖號合併三次 report 的最新結果，取得 82 張圖。 各張 SVG／PNG 路徑皆存在，文字標籤具有正值尺寸，content bounds 皆位於 viewBox，檢查 exit 0

完稿前以限定 `architecture-handbook-validation` 路徑的唯讀行程查詢確認，對應 Chrome／Node helper 行程數為 0，命令 exit 0。 查詢結果包含行程名稱與 PID 欄位，使用者 browser profile 維持原狀

### 文體與來源算例

`lint.lua --format json --profile final <11 頁>` 實際 exit 0，444 warnings 逐類核對：

- L027 × 439：依使用者示例及本次 spec 保留段末句號
- L029 × 1：Application 初始化段落描述列舉來源、註冊熱鍵及建立快照的時序，語意完整
- L053 × 4：「通過驗證／parser」表達 pass 語意，保留原文

獨立肯定式 prose／Mermaid 候選掃描與常見簡體字掃描皆為 0。 原始程式的 operators、enum、字串與註解節錄維持來源內容

主 agent 對照 `render_math.cpp`、shader、`windows_render_stack.cpp`、`frame_scheduler.cpp` 與 `EmbedSpirv.cmake` 核算：

- 1920 × 1080 放入 1000 × 800，viewport 是 `(0, 119, 1000, 562)`。 目的高度 801 時上下餘量為 119 與 120
- content／allocation 比例為 0.9375，中心 UV 為 `(0.46875, 0.46875)`
- 雙線性四權重 0.375、0.125、0.375、0.125，總和 1。 示例通道值的結果為 0.45
- CPU 邊緣示例的 `u = 0.6875`、`s = 2.25`，0.8 與零 padding 加權為 0.6。 零值證據來自 CPU 每次清零
- 以奈秒刻度示例重算 60 FPS：週期 16,666,666 ns。 49 ms 的 poll 將期限推至 49,999,998 ns，50.1 ms 的需求可再接納。 文件保留實際 `Clock::duration` 精度條件
- SPIR-V magic bytes `03 02 23 07` 依 little-endian 形成 `0x07230203u`

數值紀錄位於 `build/architecture-handbook-validation/arithmetic.json`

### 程式節錄的獨立證據 audit

`handbook_snippet_audit` 以獨立 context 完整掃描 11 頁，取得 C++ 27 段、GLSL 2 段、Lua 4 段與 CMake 1 段，共 34 段。 比對保留字串與字元 literal，另對程式外空白及說明註解做正規化。 結果 34／34 對應目前來源，命令 exit 0

30 段的差異屬於空白排版。 其餘四段在 browser helper 的 hide、runtime 的 stopped，以及兩個 GLSL shader 節錄中省略來源說明註解，程式本體與 literal 相符

Audit 另以 `C:/lua/lua.exe -E -e` 重新計算 RowPitch、viewport、四角及中心的 barycentric 重建、UV scale、雙線性權重、CPU padding、deadline 格線與 SPIR-V byte order，結果皆與教材一致。 Lua inventory 範例實際載入 repository 的 profiles、validator 與 fixtures，接受三種正確 profile，並依序回報 `Duplicate case ID 02` 與 `Missing case ID 03`。 整次計算 exit 0

這份數值證據涵蓋公式覆算及純 Lua 規則執行，C++、GPU 與硬體的執行結果沿用各自獨立的驗證流程

### 檔案範圍與 Git 保留

最新 Git 檔名集合與 manifest 都是 173 檔，雙向差異為 0，README 索引亦為 173 列。 全套 checker 確認 158 個交付範圍外的既有檔案 SHA256 相符。 `imgui.ini` 的檔案長度為 150 bytes，mtime 在起始 JSON 快照的數值精度下相符，檢查使用 metadata

HEAD 與 index SHA256 均與起始快照相符。 本次交付範圍為 11 頁架構教材與三份 spec／plan／verification 工作文件，既有程式、測試、工具、發行檔案及其他工作內容維持起始雜湊。 一次性驗證工具與圖像產物位於 Git 忽略的 build 目錄

`git diff --check -- docs/architecture` exit 0。 Git 依現有 attributes 提示 LF／CRLF 轉換資訊。 三份工作文件的 `--profile spec` lint exit 0，三項 advisories 經核對：L120 的測試範圍描述、L115 的 Tech stack 說明與 L053 的 review 成功語意
