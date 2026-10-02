# 執行階段導讀設計規格

日期：2026-09-22。範圍：`D:/document/MesRepo/Airelinna` 的 avb0795。來源基準為當日 working tree，HEAD `a862457`，包含當前 staged 的選區尺寸修正與既有 working-tree 實作。

## 目的與交付

讀者希望沿程式的執行順序建立背景知識，理解操作如何成為擷取工作、影格如何到達螢幕，以及各個 owner 何時接手和釋放資源。教材需要同時提供全域路線、可獨立閱讀的階段、可追蹤至實際函式的 unit，以及具體資料值的使用範例。

新增 `docs/execution-guide/`，入口為 `overview.md`。原有 `docs/architecture/` 保持原貌，供元件索引與深入參考。此次變更集合為新導讀、新 spec、新 plan 與本機驗證紀錄；原始碼、既有 staged changes、偏好資料和原文件各自保留現況。正式文件是否提交依本次實作完成時的明確授權與 staged 隔離條件處理；spec、plan、檢查腳本與驗證紀錄保存於本機。

「階段」以目前執行流程劃分；編號提供閱讀順序。重複 tick、非同步 callback、設定交易、recovery 支線與 build 前置流程，透過圖上的迴圈與分支表達。

## 組織選擇

| 方案 | 使用情境與取捨 |
| --- | --- |
| 依既有元件分章 | 適合按檔案定位，跨元件的一筆操作需要跳頁追蹤 |
| 單篇完整流程 | 閱讀順序連續，篇幅與巢狀結構集中於同一頁 |
| Overview＋執行階段＋H2 units（採用） | 以操作與影格的旅程定位元件，藉階段索引及 canonical structure 說明控制重複篇幅 |

使用者已授權 spec rolling review → plan rolling review → 直接撰寫。依此授權推進書面設計與 reviewer gates。

## 文件集合與 H2 units

各檔的第一張圖以該檔下列 H2 units 為節點；外部呼叫者／接收者可作帶有「外部」標籤的 context 節點。

| 檔案 | H2 units（具體標題可依實際符號微調） | 核心來源 |
| --- | --- | --- |
| `overview.md` | 程序啟動與工作建立；一張影格的旅程；互動、回饋與退出 | `main.cpp`、app ports、runtime、render stack、host |
| `01-startup-and-preferences.md` | 程序前提與服務組裝；偏好、語言與快捷鍵初始化；Controller owner 建立 | `main.cpp`、`application.cpp`、`config_store.*`、`application_language.cpp`、`desktop_host.cpp` |
| `02-commands-and-session.md` | UI 意圖與 FIFO 命令；來源識別與選取；Session 與 runtime 建立 | `controller_view.*`、`commands.hpp`、`application.cpp`、`replica_session.*`、source catalog/registry/factory |
| `03-capture-and-scheduling.md` | Capture session 與 callback admission；影格 checkout 與最新值交接；主執行緒 tick 與 deadline | `wgc_capture_job.*`、`detail/wgc_capture_state.*`、`windows_captured_frame.*`、mailbox、scheduler、runtime |
| `04-frame-transport.md` | 裝置能力與 bridge 選路；CPU copy 與 row pitch；共享 GPU image 與同步交接 | `windows_frame_bridge.*`、`bridge_policy.*`、`cpu_row_copy.*`、`vulkan_input.hpp` |
| `05-replica-render-and-present.md` | Slot 與 Vulkan submission；座標、UV 與 texture sampling；WSI 呈現與資源退休 | `windows_render_stack.*`、`vulkan_renderer.*`、`vulkan_wsi.cpp`、`render_math.*`、`replica.vert/.frag` |
| `06-settings-and-window-interaction.md` | 設定交易與偏好發布；框選、比例與滾輪幾何；額外功能與 native owner | `application.cpp`、runtime settings operations、overlay transactions/policy/adapter、browser keep-alive |
| `07-controller-loop-and-projection.md` | Runtime facts 與 view 投影；事件迴圈與 UI cadence；ImGui 圖形資源與輸入交接 | app/runtime ports、view state、localized presentation、`desktop_host.cpp`、`ui_cadence.hpp`、`imgui_input_sync.hpp` |
| `08-recovery-and-shutdown.md` | Source resize 與 graphics recovery；停止、drain 與程序退出；效能觀察與證據輸出 | runtime policies、runtime、render/bridge drain、Application stop、host、performance report |
| `09-build-and-tools.md` | CMake target 與平台組裝；Shader 與資源生成；Lua 工具與驗證入口 | root/src CMake、Dependencies/VerifySourceBoundaries、shaders、EmbedSpirv、tools、test inventory |

階段 01–08 為執行期間的不同協作面向；階段 09 為產出程式的前置流程。Overview 明示 02/03/05/06/07 之間的循環與 08 的退出／復原支線。

## 標題與固定 unit 格式

每份 markdown 採用以下結構：

1. H1：整篇目的與階段名稱。
2. 第一個內容區塊：Mermaid big picture。節點 ID 對應該檔 H2 unit，label 同時提供入口或主要操作；圖中交代跨 unit 的資料、控制、持有／借用關係。
3. 導讀文字：目的 → 需求 → 本階段如何達成；附前後階段連結、輸入／輸出、owner／thread 表格，以及圖例。
4. 每個 unit 佔一個 H2；其下三個同層 H3 固定為：
   - `### 結構與流程圖`：第一個內容區塊為 Mermaid，展開本 unit 的結構關係、資料／控制交接、持有／借用與實際 call edges。
   - `### 資料結構與使用方式`：先建立用途與需求，接著解說相關 types、member 分組、值來源、有效期間及使用範例。
   - `### 實作流程`：先給實際函式級 call graph 或 sequence diagram，再依順序穿插程式片段及解說。
5. H4 用於必要背景、數學、分支或生命週期；H5/H6 依真實的更深一層子概念使用。標題層級逐級增加。

Overview 也採三個 H2 與相同 H3 結構，但以跨階段呼叫、重要跨界資料值及導覽連結為教學對象。階段文件負責該領域的完整 member／演算法說明。

## 圖的語意契約

- `-->` 標記控制／呼叫，edge label 含 `呼叫：` 或 `控制：`；純狀態轉移標記 `狀態：`。
- `==>` 標記資料值傳遞，edge label 含實際資料型別或變數名稱。
- `-.->` 標記 `持有：`、`借用：` 或 `移交：`，並寫明 unique/shared ownership 或 borrowed lifetime。Move 是移交，不代表複製像素。
- Context、OS callback、主執行緒、GPU submission／completion 各自標出角色。Sequence diagram 用 `呼叫`、`資料`、`完成證據` 文字辨識邊的語意。
- Ownership edge 指向擁有的資源；callback invocation、抽象 port 呼叫及 concrete adapter 的實作對應各有明確標籤。
- Call graph 以實際呼叫為準；純閱讀先後或資料依賴以對應 label 表示。第一張 big picture 可聚合操作，unit 級圖使用實際符號。
- 每張圖保留足夠小的範圍供單頁閱讀；複雜同步拆成 unit 圖與實作順序圖。

## 資料結構與程式片段契約

每個 unit 選擇真正參與流程的 types，建立「目的、members、來源／使用者、生命週期」說明。首次完整介紹為 canonical 定義；後續出現的 unit 提供本地用途、此路徑相關 members、短範例與 canonical 連結。

表格按 invariant／責任分組 members，說明它們共同維持的關係。例如 submission record 的 fence、slot identity、phase 一起證明何時可重用。簡短 struct 可列出全部 members；較大 owner 聚焦本 unit 存取的 members，附 definition 連結，並明示「本 unit 使用的 members」。

每 unit 至少提供一個資料使用範例與一個實作片段。所有片段帶 `cpp`、`glsl`、`cmake`、`lua` 等正確語言標籤，以及實際函式／結構名稱與原始檔相對連結。省略的程式碼使用 `...` 或 `// ...`，並於片段前標示「節錄」。示意組裝標示「使用示例」及前提；private record 的示例在 owner 內部語境解釋。完整可獨立執行的承諾只適用於明示為完整範例的區塊。

每個具體行為可由目前 working-tree source 查證。以資料結構解說補充語言背景，例如 reference 借用、unique_ptr 移交、shared observation lease、optional 值、variant 命令、expected 結果，再串回實際操作。

## 背景知識與演算法深度

在用到術語的 unit 先提出目的與需求，再引入術語。範圍包含：

- COM／DPI／GLFW 的初始化需求與 composition root 的反向生命週期。
- FIFO 操作順序、SessionId 世代、source identity 與 observation lease。
- Callback admission／active-user 計數、latest-value mailbox、scheduler deadline 與合併影格。
- WGC borrowed frame、D3D copy、CPU row pitch、共享 GPU image、external handle、同步證據。
- Vulkan command buffer／fence／semaphore／swapchain 的具體角色；controller 和 replica 各自持有 graphics resources。
- Crop intersection、letterbox 比例、UV 映射、雙線性取樣、color／alpha 的實際設定。數學附變數單位、公式、可計算的小例子及對應程式片段。
- Settings transaction、snapshot／rollback、client／outer geometry、work area、wheel scaling。
- Resize 與 device-loss recovery、drain／Stopped／AbandonProcess 的銜接。

演算法採目前原始碼實際使用的方法。外部 API 的額外保證若寫入教材，附官方來源；已在本地程式中明確指定的參數與分支直接連至 source。

## 語言與閱讀體驗

採繁體中文與保留必要英文符號。正文以正向敘述呈現目的、需求、責任與結果；沿用使用者對正向句型的要求，將限制表達成有效條件或責任範圍。語法中的 `!`、`nullptr`、`std::nullopt`、`= delete`、原始錯誤 enum 與原文 source identifiers 保持其實際意義。解說用的片段註解採正向語句。

條件集合直接列點。每張圖後的文字解釋讀圖順序及關鍵關係，並補上 edge 背後的原因。新概念保持「目的 → 需求 → 結構／機制 → 具體例子」的推進。

## 驗收與 review

1. Spec 自查完成後由全新 subagent review；root 按實際 source／需求查證 findings。修正真實 Critical／Medium 後，再開全新 reviewer，直到 0 Critical／0 Medium。
2. Spec 通過後建立 plan，以相同 rolling review gate 驗收，再開始正式教材。
3. 教材完成後檢查檔案集合、H1／首張圖、H2 與圖對應、三個 H3、每 unit 的三類內容、範例與 source links、Markdown fences／相對連結、Mermaid 語法和術語／正向句型。
4. 人工核對代表性路徑：Start、frame delivery、CPU/shared bridge、present retirement、crop/reset、Stop/recovery。補充小型本機檢查工具；驗證範圍聚焦文件結構與真實來源。
5. 跨階段整合由全新 reviewer 檢查誤導性架構描述、錯誤 ownership、虛構 calls、缺失的背景及 member 使用前提；有效 Critical／Medium 依 rolling 規則處理。
6. Reviewer 完成後釋放支援的 agent 資源，持續使用全新 review identity。若工具介面提供的操作保留 completed thread，精確回報工具狀態；review gate 狀態寫入本機紀錄。

本次文件工作以 source／文件驗證為主要證據。GUI、hardware capture、偏好資料及現有 build 由既有 owner 保持原狀。
