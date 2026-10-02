# 架構教材設計

本次交付目的是讓讀者從程式入口追蹤一次操作與一張影格，並透過逐檔索引找到各責任的實作。 文件描述目前 working tree 的程式，以來源連結、流程圖、契約與數學推導構成閱讀路線

## 交付範圍

沿用 `docs/architecture/README.md` 作為總入口，更新既有八份元件文件，新增瀏覽器保活與 Lua 工具兩份元件文件。 入口頁提供完整檔案索引，各元件頁提供小元件詳解

| 文件 | 元件與責任 |
| --- | --- |
| `docs/architecture/README.md` | Big Picture、控制與資料路徑、所有權、執行緒、導讀路線、完整逐檔索引 |
| `docs/architecture/core.md` | 可攜型態、session 與 application state、deadline scheduler、latest-frame mailbox、hotkey values |
| `docs/architecture/application-ui.md` | commands、ports、application use cases、設定儲存、view projection、UI widgets |
| `docs/architecture/localization.md` | 語言標籤解析、requested/effective language、catalog、在地化投影、語言選單與 OS query |
| `docs/architecture/windows-capture.md` | 來源列舉、registry 身分、lease、WGC job、callback gate、checkout 與尺寸重建 |
| `docs/architecture/windows-runtime-overlay.md` | runtime factory、tick、graphics recovery、drain、overlay、Win32 系統轉接器 |
| `docs/architecture/browser-capture-keep-alive.md` | 額外功能的政策、程序內 window class／透明 helper HWND、啟用與退休，連回 runtime 呼叫端 |
| `docs/architecture/renderer.md` | CPU/GPU transport、slot、Vulkan presentation、幾何映射、取樣與 SDR 色彩路徑 |
| `docs/architecture/desktop-host.md` | main 組裝、controller graphics、字型、事件迴圈、關閉與程序退出 |
| `docs/architecture/build-and-validation.md` | CMake targets、依賴與 patches、shader 生成、資源、測試與分層檢查 |
| `docs/architecture/lua-tooling.md` | CMake launcher、Lua CLI、JSON decoder、profiles、validator、測試 harness |

既有 `docs/development/` 維持操作指南的角色，元件文件連回對應操作指南。 本次 spec、plan 與驗證紀錄各有獨立工作文件

## 逐檔索引的邊界

索引集合取自 `git ls-files --cached --others --exclude-standard`，對路徑去重並確認檔案存在。 2026-09-09 初始集合為 168 個檔案。 完稿時重新計算集合，包含本次新增文件，讓索引與交付快照一致

每個檔案在索引有一列，欄位是可點擊的 repo-relative 路徑、責任摘要、元件詳解。 分組依 canonical 責任進行，跨元件合作透過詳解連結呈現。 測試、資源、第三方 patches、vendor、設定與開發文件各自分組。 工作紀錄組收錄 spec、plan、verification 與 issue 記錄的名稱及用途

建置目錄、下載的依賴、Git 內部資料與忽略的產物由生成流程圖描述。 本機 `imgui.ini` 以檔名與偏好資料角色登記，內容保持私有。 已追蹤且符合 ignore 規則的檔案仍依 Git 清單收錄。 最終索引以集合相等檢查覆蓋率及重複列

## 三層閱讀結構

入口頁在 H1 後先呈現 Mermaid Big Picture，圖中標明 `main`／`run_application`、`DesktopHost`、`Application`、queue／commands／view、domain state、native services、capture／renderer／overlay 與畫面目的地。 第二張圖展開所有權，第三張圖展開執行緒交接。 檔案清單依 H3 分組，讓主頁的 H2 維持清楚閱讀階段

每份元件文件在 H1 後先呈現元件 Mermaid，接著介紹目的、需求、入口、總控制流、資料流與重要型態。 每個 H2 表示一個有實際責任的小元件，在標題後先呈現 Mermaid，接著展開機制。 H3 用於演算法步驟、程式片段、契約與數學推導

圖中的節點可表示程序、函式、owner 或值型態，標籤帶上用途。 箭頭依關係標示「控制：方法／命令」、「資料：型態」、「持有」或「借用」。 sequenceDiagram 以訊息文字標示呼叫、值傳回與 completion evidence。 每張圖附實際來源連結，圖中 owner 與借用關係由宣告及建立／銷毀位置核對

每個小元件說明包含：

- 達成的目的與所需條件
- 入口函式、呼叫者、主要操作順序與輸出
- 關鍵型態之間的識別、持有、借用或投影關係
- 有狀態操作的成功、重試、失敗與清理路徑
- 與鄰接元件的責任交界及來源連結

## 技術深度

為了讓取樣結果可以由讀者自行推導，renderer 文件依現行程式展開下列鏈條：

1. `aspect_fit` 的 64-bit 交叉相乘、限制軸判斷、整數截斷與置中，附具體尺寸算例
2. `replica.vert` 的三個超大三角形頂點、`gl_VertexIndex`、clip space 與正高度 viewport，連結 `vkCmdDraw(..., 3, 1, 0, 0)`
3. `content extent / allocation extent` push constant、插值後 UV 與有效內容範圍，區分 viewport 幾何與 texture 範圍
4. `VK_FILTER_LINEAR`、`CLAMP_TO_EDGE`、base mip、texel 中心與雙線性權重公式，說明 allocation 邊界與 content 邊界的實際關係
5. fragment 取樣值、UNORM 與 SDR nonlinear surface 的搭配，描述 alpha／blend 設定及適用的色彩契約

以上包含實際程式節錄、變數定義、數學式、數值算例與選擇原因。 節錄保持原有型態、常數及運算式，簡化的範例明確標示為推導示例。 輔助的 Vulkan 語義依 Khronos 官方資料查證並就近連結

其他重要演算法依各元件展開：

- Core：deadline 選擇、frame-limit 排程、mailbox 最新值交接
- Application：command 順序、runtime event 投影、設定讀取／驗證／atomic replacement
- Capture：source identity／generation、epoch admission、pending intake、callback quiescence、checkout 生命週期
- Runtime：建構後發布、資源重建順序、aggregate drain 與進展期限
- Overlay／features：互動模式交易、熱鍵替換、保活 eligibility／hidden proof／退休條件
- Renderer：三個 slot 的 CPU/GPU 橋接、sequence 選擇、submission 與 presentation 各自完成證據、surface／device recovery
- Localization：語言標籤匹配、系統清單順序、requested/effective 決策與 catalog lookup
- Desktop：主迴圈 deadline／wake、字型 byte ownership、controller graphics 與終止路徑
- Tooling：CLI 分支、JSON container 標記、可信 profiles、inventory 比對、依輸入再依 profile 順序累積診斷、pipeline 兩端 exit status

## 文體契約

正文使用臺灣繁體中文，保留可搜尋的英文識別字。 首次引入概念採「目的 → 需求 → 結構與機制」順序。 先以目的與需求引導讀者，再命名與解釋型態。 明確的條件清單使用列點

所有新增正文、圖中文字、表格說明與新增註解採肯定式表達。 掃描字詞 `才`、`只`、`不`、`沒`、`未`、`無`、`非`、`勿`、`莫` 以定位候選句，再以語義逐段檢查。 檔名、原始程式識別字、運算子、API 字面值與忠實原始程式碼節錄保留原值，節錄周邊解說遵循肯定式契約。 清理路徑用持有條件與完成證據描述，錯誤路徑用具體結果與後續操作描述

每段維持連續文字，繁體中文句子採自然句號。 此處依使用者範例選擇段末句號，Lua linter 的 L027 結果採文體註記處理。 大小寫、符號與程式碼採來源精確值。 技術說明保持資訊密度，段落各自增加一項可追蹤的機制或原因

## 工作邊界與驗證

交付修改集中在上述 11 份架構文件與本次 spec、plan、verification 記錄。 原有產品、測試、工具、publish 與其他工作紀錄內容持續保留。 HEAD、Git index 與範圍外檔案雜湊在開始及結束比對

驗證分成五組：

1. 完整檔案集合與索引集合相等，每個路徑一列，所有本機連結存在
2. 主圖／元件主圖／每個 H2 的圖皆符合版型，Mermaid 使用本機可用 parser 驗證。 圖形幾何另以人工審查可讀性，驗證紀錄分別記載解析與視覺檢查的實際方法
3. 全文肯定式檢查、目的優先的閱讀順序、術語與繁體中文 Lua lint。 每個 warning 依脈絡修正或記錄採用理由
4. 來源核對：入口、型態名稱、所有權、控制／資料箭頭、錯誤與完成證據、程式片段、公式與數值算例
5. 範圍比對：HEAD、index 與架構交付範圍外的既有檔案維持起始雜湊

本次沿用現有測試集合及建置成果作為閱讀參考，文件驗證以五組檢查完成。 暫時的檢查輔助放在忽略的 `build/architecture-handbook-*`，持續維護的工具架構保持原狀

## Rolling review

Spec 完成後，每輪建立新的獨立 reviewer，檢查整份設計與來源證據。 主 agent 逐項確認 finding，記錄採納理由及修正。 critical／high／medium 數量歸零後進入 plan

Plan 完成後，以相同的新 reviewer 流程檢查任務覆蓋、檔案界線、執行順序、驗證方法與工作量。 清楚通過兩個 gate 後接續實作，實作完成再進行來源準確度與文體審查。 Reviewer 完成後結束執行，讓並行席次供後續工作使用。 每輪的證據與結論記入本次 verification 文件
