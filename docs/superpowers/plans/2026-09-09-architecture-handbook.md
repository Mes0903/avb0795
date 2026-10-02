# 架構教材實作計畫

> 執行方式：主 agent 協調獨立元件文件工作，完成規格符合度 review 後進入來源品質 review。 各 reviewer 採 fresh context。 文件變更保留於 working tree，產品與 Git index 維持起始狀態

**Goal:** 交付中文架構教材，讓讀者沿入口追蹤控制流、資料流、資料結構與演算法，並透過完整逐檔索引找到程式責任

**Architecture:** 總覽 → 元件 → H2 小元件。 沿用九份架構文件，新增保活與 Lua 兩頁。 每個元件／H2 以 Mermaid 開頭，再依目的、需求、機制、程式與數學展開

**Tech stack:** 文件採 Markdown 與 Mermaid，閱讀來源涵蓋 C++23、GLSL、Lua 與 CMake。 驗證使用本機 Node Mermaid parser、繁體中文 Lua linter，以及 Git 檔名集合與 SHA256

依據：[已完成兩輪 review 的 spec](../specs/2026-09-09-architecture-handbook-design.md)。 證據寫入 [verification](../../verification/2026-09-09-architecture-handbook.md)

## Task 1：Plan gate 與工作快照

- [x] 建立新的完整 plan reviewer，檢查本文所有任務與 spec 對應。 每個 finding 由主 agent 依來源驗證、修正，再啟動新 reviewer。 critical／high／medium 歸零後接續 Task 2
- [x] 使用目前 `p4-application-release` working tree，保留此處的在地化、工具與註解成果。 核對 `build/architecture-handbook-validation/baseline.json`、HEAD 與 index，確認文件範圍外的基準完整
- [x] 確認以下工具可執行，記錄實際版本及 parser smoke：

```powershell
& 'C:/lua/lua.exe' -v
& 'C:/Program Files/nodejs/node.exe' --version
& 'C:/Program Files/nodejs/node.exe' --no-warnings build/architecture-handbook-validation/probe-mermaid.mjs
git rev-parse HEAD
Get-FileHash -LiteralPath '.git/index' -Algorithm SHA256
git ls-files --cached --others --exclude-standard
```

Parser 使用本機 VS Code extension 的 Mermaid bundle。 Node process 內以 identity sanitizer 配合 grammar 檢查，原始套件檔案保持原值。 檢查成果記為 parse evidence，SVG 視覺結果另有獨立的證據欄位

## Task 2：撰寫四組教材

每組先閱讀列出的程式及原有文件，以呼叫者、owner、資料值、失敗與清理路徑核對圖與文字。 每頁 H1 後第一個內容為 Mermaid，每個 H2 後第一個內容亦為 Mermaid，H3 放推導、程式片段與算例。 各組修改限於自己列出的文件，來源維持原狀

### A：Core、Application／UI、在地化

檔案：更新 `docs/architecture/core.md`、`application-ui.md` 與 `localization.md`

閱讀：`include/airelinna/{core,app,i18n,ui}` 與 `src/{core,app,i18n,ui}`，另含 `platform/windows/windows_ui_language.{hpp,cpp}` 與 `detail/ui_language_query.hpp`

- [x] Core：型態／識別、ApplicationState、ReplicaSession、FrameScheduler、LatestValueMailbox、Hotkey 各建 H2。 寫出 scheduler 的 deadline 跳格公式與算例、mailbox 的鎖內 move／鎖外釋放及 ready edge
- [x] Application／UI：command queue、ports 與 orchestrator、runtime event 優先序、設定交易與 ConfigStore、view projection、widgets 各建 H2。 展開 Start／Stop／Interrupted replacement、設定成功與持久化的時點、requested language 儲存欄位
- [x] 在地化：language values／tag matcher、ordered system query、requested／effective resolution、catalog／localized presentation、language selector 各建 H2。 字型所有權連到 desktop 頁
- [x] 每個重要演算法加入實際節錄或精確的步驟算例，完整保留 session／language／runtime 的責任分工

### B：Capture、Runtime／Overlay、保活

檔案：更新 `docs/architecture/windows-capture.md`、`windows-runtime-overlay.md`，新增 `browser-capture-keep-alive.md`

閱讀：`platform/windows` 的 source／registry／capture／job、runtime／factory／overlay／system adapters、`detail`、`features`，以及 `include/airelinna/app/ports.hpp`

- [x] Capture：catalog／registry、source lease／publication、WGC state／job、intake gate、captured frame、resize 與 close 各建 H2。 圖中串起 callback、epoch、pending、mailbox、quiescence 與 checkout owner
- [x] Runtime／overlay：factory publication、tick／settings、graphics recovery、aggregate drain、OverlayWindow、Win32OverlayAdapter／transactions、system adapters 各建 H2。 System adapters 以 H3 及小圖展開 hotkey、capture exclusion、DPI、atomic replace。 保活入口連到獨立 feature 頁
- [x] 保活：eligibility policy、helper native owner、runtime 啟用控制、hide proof 與 delayed retirement 各建 H2。 指出 class ATOM、HWND、觀察到的可見性、250 ms grace、checked release，並以肯定式條件說明支援情境
- [x] 三頁交接一致：published runtime 留到 Stopped，source loss 的 domain 結果、callback admission 與 GPU completion 各由具體 owner 證明

### C：Renderer 與 Desktop host

檔案：更新 `docs/architecture/renderer.md`、`desktop-host.md`

閱讀：`render` 全部檔案、`shaders/replica.{vert,frag}`、`src/desktop`、`include/airelinna/desktop/desktop_host.hpp`、`main.cpp` 與 `platform/windows/controller_font.{hpp,cpp}`

- [x] Renderer：能力／bridge policy、CPU copy、GPU interop、ready／retained selection、submission／presentation、WSI recovery、aspect-fit、texture mapping／sampling 各建 H2。 必要的色彩說明放在 sampling H3
- [x] 數學依實作推導：64-bit cross product 與 viewport 算例、超大三角形與 barycentric UV、content／allocation scale、texel-center 座標、四 texel 雙線性公式與算例。 說明 base mip／CLAMP_TO_EDGE 的 allocation 邊界，並核對有效內容邊緣的實際取樣行為
- [x] 展示實際 shader、push constant、sampler／blend／surface 設定片段，連到原始程式碼。 API 語義查 Khronos 官方原始資料並就近連結，推導式標明變數與適用條件
- [x] Desktop：composition／startup、controller graphics、font／atlas、event loop、completion records／WSI、shutdown 各建 H2。 展開 queue 下一輪處理、wake／deadline 最小值、font staging 與 sampled image 各自生命週期、`HostRunResult`

### D：主 agent 整合總覽、逐檔索引與工具

檔案：更新 `docs/architecture/README.md`、`build-and-validation.md`，新增 `lua-tooling.md`

閱讀：`main.cpp`、ports／commands／application／desktop headers、root／src／shaders／tests CMake、`cmake/*`、`tools/*`、`tests/tooling/*`、現有 development 指南。 資源及本機偏好以角色與引用關係盤點

- [x] README 開頭 Big Picture 由 main 接到 controller、commands、Application、catalog／factory、runtime、capture、renderer、overlay。 另以 ownership 圖與執行緒圖展開值資料、native checkout 與 completion evidence
- [x] 檔案表以元件為 H3 分組，每個 path 一列，責任與詳解各一欄。 函式宣告／實作檔分列，tests、vendor、patches、resources、docs／records 與本機偏好分組
- [x] Build：target graph、dependency／patches、shader embedding、boundary／test gates、editor／resource metadata 各建 H2。 依 `src/CMakeLists.txt` 展示 App／UI／Windows 對 i18n 的 direct links
- [x] Lua：launcher、CLI／decoder、profiles、pure validator、tooling harness 各建 H2。 展開 native pipeline 與雙 exit code、JSON object／array／null、4 MiB text limit、輸入與 profile 走訪順序及 12-group harness
- [x] 每組完成後交給主 agent 統一術語、來源連結、跨頁責任與圖例。 第一組至第三組可各由一個獨立 writer 並行，主 agent 同時處理本組

## Task 3：五組文件驗證

- [x] 在忽略的 `build/architecture-handbook-validation/` 維護一次性 Node checker。 取得新的 Git 清單，與 README 索引第一欄的路徑集合比對，確認每個 path 一列、名稱與內容數量相等。 本次預期 173 檔，實際結果以完稿清單為準
- [x] Checker 檢查 11 頁的 H1／H2 開頭 Mermaid，使用已 smoke 的 `mermaid.parse()` 逐圖解析。 逐個檢查 repo-relative link 及 fragment，更新同組交叉連結並確認 README／development 導向架構頁的連結
- [x] 主 agent 以 `build/architecture-handbook-validation/render-docs.mjs` 啟動本機 loopback 預覽，使用獨立 profile 的 headless Chrome 與原始 Mermaid browser bundle 渲染全部圖。 SVG、PNG、圖數與 bounds 證據存入同一個忽略目錄。 逐頁檢視圖像總覽，密集圖再檢視完整尺寸，確認標籤完整、節點間距、箭頭方向與閱讀尺度。 完成後關閉該次 browser 與 server，既有使用者 browser profile 保持原狀

```powershell
& 'C:/Program Files/nodejs/node.exe' build/architecture-handbook-validation/render-docs.mjs
```

圖形修改後，對更新頁重新渲染及檢視。 驗證紀錄逐頁寫出圖數與檢視結果，讓語法解析、實際 SVG 渲染與人工可讀性各有證據

- [x] 掃描候選字詞並逐段人工確認肯定式語意，保留原始程式字面值。 套用以下 lint，逐項記錄 warning 處理：

```powershell
$handbookPages = @(Get-ChildItem -LiteralPath docs/architecture -Filter '*.md' | ForEach-Object { $_.FullName })
& 'C:/lua/lua.exe' 'C:/Users/Mes/.codex/skills/zhTW-translocalizer-skills/scripts/lint.lua' --format json --profile final @handbookPages
& 'C:/lua/lua.exe' 'C:/Users/Mes/.codex/skills/zhTW-translocalizer-skills/scripts/lint.lua' --format json --profile spec docs/superpowers/specs/2026-09-09-architecture-handbook-design.md docs/superpowers/plans/2026-09-09-architecture-handbook.md docs/verification/2026-09-09-architecture-handbook.md
```

- [x] 主 agent 核對程式節錄與來源，重新算一次 viewport／UV／雙線性／scheduler 算例。 所有來源術語維持精確符號，文件推論標明推導條件
- [x] 依 baseline 比對交付範圍外的每個既有檔案及其集合，對 `imgui.ini` 比對 metadata。 比對 HEAD、index SHA256。 保留 11 頁及本次三份工作文件以外的內容

## Task 4：完成文件的兩階段 review

- [x] 新 reviewer 完整審查 spec 符合度，包含目的優先、圖與 H2、每檔摘要、深度及肯定式文體。 Findings 由主 agent 確認，修正後以新 reviewer 重跑至 critical／high／medium 歸零
- [x] 新 reviewer 完整審查來源準確度及分層責任，核對圖、資料結構、程式片段、演算法與 completion ownership。 採相同 rolling gate
- [x] 每輪完成後結束 reviewer 執行，記錄結果。 最後重新執行 Task 3 的機械驗證與更新圖像檢視，將實際解析、SVG／PNG 渲染、人工可讀性／語意檢查、文體 warning 與檔案範圍結果寫入 verification
- [x] 在交付中提供總入口與代表性的演算法頁連結，說明實際完成的檢查及文件變更範圍
