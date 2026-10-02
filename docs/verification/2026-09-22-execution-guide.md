# 執行階段導讀驗證紀錄

日期：2026-09-22。Repo：`D:/document/MesRepo/Airelinna`，產品 `avb0795`。

## 交付範圍與保護基準

正式教材為 [overview](../execution-guide/overview.md) 與九篇執行階段 Markdown，共 30 個 H2 units、70 張 Mermaid 圖。各 unit 使用三個同層 H3：結構與流程圖、資料結構與使用方式、實作流程；H4 展開背景、算法與生命週期子操作。

來源基準為當日 working tree，包含原有選區尺寸修正。起始 HEAD 為 `a86245792d1dd72a3d430fae4d29869762e05c52`；index SHA256 為 `D07D53BBA95CAE4BA5414E04A874F7C91DD5A8D6586B22F43D0DCB06DB33BEB5`。本機 `build/execution-guide-validation/baseline.json` 保存 174 份既有檔案的 SHA256。檢查器逐項核對原始檔、既有文件、HEAD 與 index；2026-09-22 檢查全部相符。

既有 staged 路徑維持六項：

- `docs/development/region-selection.md`
- `platform/windows/detail/overlay_policy.cpp`
- `platform/windows/detail/overlay_policy.hpp`
- `platform/windows/win32_overlay_adapter.cpp`
- `platform/windows/win32_overlay_adapter.hpp`
- `platform/windows/windows_replica_runtime.cpp`

本次教材、spec、plan、此紀錄皆以本機工作樹交付；驗證 helper 與圖像放在 `build/execution-guide-validation/`。

## Rolling review

| Gate | 全新 reviewer | 結果 |
| --- | --- | --- |
| Spec | `execution_guide_spec_review_r1` | 0 Critical／0 Medium；root 修正三處字形 |
| Plan | `execution_guide_plan_review_r1` | 0 Critical／0 Medium |
| 全套教材 R1 | `execution_guide_full_review_r1` | 0 Critical／2 Medium，逐項核實後修正 |
| 全套教材 R2 | `execution_guide_full_review_r2` | 0 Critical／1 Medium，逐項核實後修正 |
| 全套教材 R3 | `execution_guide_full_review_r3` | 0 Critical／0 Medium，完整來源與跨章契約核對通過 |

R1 的兩類問題及 source 證據：

1. 具名 API／member：透明度入口為 `ControllerView::render_contents`；host 方法屬於 `DesktopHost::Impl`；runtime 結果使用 `next_wake`；runtime settings 方法為 `apply`；程序結果為 `HostRunResult`。Root 分別核對 `src/ui/controller_view.cpp`、`src/desktop/desktop_host.cpp`、`include/avb/app/ports.hpp`、`src/app/application.cpp`、`include/avb/desktop/desktop_host.hpp` 後修正圖與文字。
2. 跨層身分：依 `render/vulkan_input.hpp` 將 overview 的 `SlotKey` 與 `FrameStamp` 拆列實際 members；capture generation 歸回 `WindowsCapturedFrame`／allocation 契約。

同輪 Low 的 UI cadence 順序亦依 `DesktopHost::Impl::render_controller` 核實：`did_build` 位於 `attempt_present` 與 native iconify 之前。Root 自查另補上 `WindowsControllerWindow::create` 結果型別到基底 `unique_ptr` 的移交說明。

每輪 reviewer 使用獨立 identity，完成即結束該次工作。工具保留 completed thread 的紀錄；後續 gate 使用新的 reviewer。

R2 指出 overview 的 `take_latest` 邊界：`WindowsReplicaRuntime::tick` 呼叫 `WgcCaptureJob::take_latest`，它經 Impl／FrameCallbackState 呼叫 `LatestValueMailbox::take`。Root 核對兩個 `.cpp` 與 mailbox header 後，將 public job 節點、內部委派與 optional frame 移交補入兩張圖及解說。

R3 完整重讀十篇、spec 與 plan，並核對來源的 publication、callback、兩條 transport、retirement、settings／region、recovery／drain 與 Lua pipeline。該 reviewer 確認修正後的封裝邊界、三層完成證據、owner 釋放順序及跨章資料契約，最終 gate 為 0 Critical／0 Medium。

## 結構、連結與敘述檢查

執行：

```powershell
node build/execution-guide-validation/check-guide.mjs
git diff --check -- docs/execution-guide
```

結果：10 files、30 units、70 diagrams、372 links、174 protected files、0 style candidates、0 errors，exit 0。

檢查範圍包括固定頁面集合、H1／H2／H3 層級、首圖與 H2 unit 名稱對應、各 unit 的圖／member 表／使用示例／實作片段、fences、相對連結與行尾空白。正向敘述與字形掃描涵蓋正文及 Mermaid，程式碼保留原始 operator 與 identifiers。三份記憶體 mutation（H3、link、unit diagram）皆被檢查器辨識。

## Mermaid 實際渲染與圖面檢查

執行：

```powershell
node build/execution-guide-validation/render-guide.mjs --all
```

最終 report：`build/execution-guide-validation/visual-1790083928575-66592/report.json`。70 張 Mermaid 在獨立 headless Chrome、專用 profile、loopback server、strict DOM 下完成 SVG／PNG；10 份 contact sheets 已產生。所有圖的 content bounds 均在 viewBox 範圍內。

Root 檢視十篇 contact sheets，並放大查看密集圖。針對 01 啟動首圖、06 settings 首圖、07 loop 首圖與 overview 互動圖的重疊標籤，改以明確 owner／資料節點排列；03 checkout 圖轉為 TB，tick 圖分開呼叫與結果彙整。調整後以原尺寸 PNG 複查。09 Lua sequence 的 `0;0` Mermaid 語法分隔改為「兩個結果皆為 0」，CMake 程式節錄保留實際值。

Renderer 由既有本機 helper 建立新副本，使用已安裝的 standalone Mermaid bundle `mermaid.core-70b6fe46fa1f.js`。初始 wrapper bundle 的 CSS preload 問題以選用相容 standalone bundle 解決。瀏覽器以專用背景程序執行，profile 與連線由本機 helper 管理。

## 來源與數學證據

作者、root 與獨立 reviewer 核對 Start／publication、callback admission／mailbox、CPU 與 shared producer completion、slot／present retirement、crop／reset geometry、settings rollback、recovery／drain、host loop、CMake／Lua 主線。Root 另核對 `intake_claimed_frame` 的 mailbox 呼叫與 `FrameObservation` 資料路線。

Node assert 覆算：

| 範例 | 結果 |
| --- | --- |
| 1920×1080 → 1000×800 aspect fit | `(0,119,1000,562)` |
| allocation 1000×800，crop `(100,50,400,200)` | offset `(0.1,0.0625)`、scale `(0.4,0.25)` |
| texel-center bounds | min `(0.1005,0.063125)`、max `(0.4995,0.311875)` |
| Bilinear `a=0.25,b=0.5` | weights `0.375,0.125,0.375,0.125`，總和 1 |
| source 3×2，pitch 16；destination 4×3 | source span 28 bytes，copied 24、cleared 24 bytes |
| client `(100,100,900,550)`，來源 2:1，work area 1920×1040，零 insets | fit `(100,125,900,525)`；96 DPI、正向一格 5% wheel `(80,115,920,535)` |

本次驗證屬文件結構、來源契約、公式與圖像檢查。C++／GLSL 節錄以 source 對照驗收；產品 binary、互動操作及實際 GPU 效能屬原有執行驗證流程。
