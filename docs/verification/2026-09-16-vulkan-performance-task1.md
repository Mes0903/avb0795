# Vulkan performance Task 1 實作驗證

日期：2026-09-16，Asia/Taipei。專案：`D:/document/MesRepo/Airelinna`，產品 avb0795。

## 範圍與狀態

本次實作範圍為已驗收 plan 的 Task 1：固定容量效能觀測、CLI、native owner 接線與操作指南。Task 1 的整體完成條件包含硬體與基線驗收；後續 Task 2–7 依這個 gate 推進。

本次新增兩個 diagnostics 檔案，調整 composition root、DesktopHost、Windows factory／runtime、現行 render stack 與 CMake／boundary checker。Application、Core、ControllerView 的功能介面維持原樣。測試集中於既有 case 08 的兩個 SECTION，top-level inventory 維持 15。

Git 起始基線為 `p4-application-release`、HEAD `a955d2e`。原有工作樹含 namespace／產品改名、UI、opacity／crop、preferences 與文件。2026-09-16 使用者明確同意硬體擷取測試與 production 前置基線 commits 的分階段整理；後續專案範圍內測試直接執行。首個候選 index 為從 HEAD 轉換的純改名，工作樹原有功能與本次 instrumentation 各自保留。

## 程式與精準驗證

- 變更前：Release production 與四個 regression executables 建置通過，非硬體 12/12 通過。
- Collector RED：兩個新增 SECTION 對 stub 的 `write_new` 預期失敗。
- Collector GREEN：case 08 共 411 assertions 通過，包含 69.9 s 停止、70.1 s drain 完成的截止邊界。
- 變更後：Release production 與四個 regression executables 建置通過；review 修正後非硬體 12/12 通過。
- CLI smoke：scope 缺 report、report 缺值、enum 錯值、重複 bridge、未知選項、目的路徑已存在六種情境，皆於 native startup 前以 exit 1 結束。
- Source boundary check exit 0；inventory verifier 確認 cases 01–15。
- Hardware cases 13–15：初次執行依 consent gate 為 skipped；使用者授權後，case 14／15 通過，case 13 的 Closed 通知等待有五個 assertion 失敗。修正測試 fixture 的 native message pump 後，由 root 重新串行執行完整套件，15/15 通過，其中硬體 3/3 實際通過，總計 3.83 秒。

## Review 與查證

### Native 接線早期 audit

Reviewer：`/root/perf_native_audit`。0 Critical／2 Medium。

1. Stop initiation 至 drain completion 可能跨越量測截止點。Root 核對 `request_stop`、host Quit 與 `finish` 的呼叫順序，確認缺少停止起點。修正為 `observe_stop`，分離 workload stop 與 drain 結果；原生 owner 的 release proof 維持原流程。
2. Controller 的最後 destroy observation 發生在容器 erase 前。Root 核對 frame／present progress，確認下一個量測窗口可能沿用舊 occupancy。修正為 host 每輪 graphics progress 後重新讀取 live 容器大小。

### Collector 獨立檢查

Reviewer：`/root/perf_collector_impl/collector_spec_review`。Collector／兩個 SECTION 的規格檢查通過，另行執行 411 assertions 通過。檢查範圍為 collector。

### Task 1 完整 spec review 第一輪

Reviewer：`/root/perf_task1_spec_review_1`。0 Critical／1 Medium。

Replica 的最後 present retirement observation 同樣位於 `record.reset()` 前。Root 查證後，在 render tick 結尾回報實際 slots／presents occupancy，讓後續窗口取得完成退休後的狀態。修正後重新 build 並跑非硬體 12/12。

### Task 1 完整重審

第二輪 reviewer：`/root/perf_task1_spec_review_2`。完整程式規格重審達成 0 Critical／0 Medium，確認 stop initiation 與兩個 owner 的 occupancy 修正。

全新 quality reviewer：`/root/perf_task1_quality_review_1`。完整程式品質檢查達成 0 Critical／0 Medium；檢查 collector、CLI／borrow lifetime、native 計數、timestamp／retained redraw、metadata／stop／固定窗口、CMake boundaries、兩組新增 SECTION 與使用指南。此輪採唯讀檢查，驗證執行由 root 的 build／test 與量測紀錄提供。各輪 reviewer 完成後皆停止工作。

### 硬體 fixture 的訊息交付

`tests/windows/wgc_hardware_test.cpp` 的 `wait_for_condition` 原先由 condition variable 等待；`GraphicsCaptureItem::Closed` 的交付需要建立 item 的執行緒持續處理 native messages。對照實驗顯示，兩次 closure flag 在 `PeekMessageW`／`DispatchMessageW` 前為 false、後為 true。修正將 native message pump 放入原等待迴圈，保留原本五秒 deadline 與所有斷言；CV 等待切為最多 50 ms，`WM_QUIT` 重新發布並結束等待。

這次變更位於本機測試 fixture。產品既有 DesktopHost 持續執行 GLFW event pump。原版 case 13：112 assertions、5 failures；修正後 case 13：112 assertions 通過；root 完整串行 15 cases 通過。測試仍保留於工作樹。

## B1 量測與產物

目錄：`artifacts/vulkan-perf/task1-20260916-4eec1ed5/`。

- `source-before-task1.json`：128 個本次變更前來源檔 SHA256。
- `source-instrumented-baseline.json`：初步量測執行檔對應的來源集合。
- 根目錄 `B1-01` 至 `B1-03`：初步 controller-only smoke。JSON complete=true；外層腳本取得 child exit code 時回傳空值，另有編譯負載交錯，因此保留為功能證據。
- `verified-controller/`：保存修正後的執行檔、source hashes 與重跑報告。外層腳本保留 process handle，已以 CLI exit 1 驗證 exit-code 取得方式。量測期間暫停 build。
- 本次啟動副本的 working directory 與 executable-relative settings 位於產物目錄，日常設定保持原位置。

環境觀察：Windows 25H2、build 26200.9445；balanced power plan；報告回傳 NVIDIA GeForce RTX 3070 Ti、Vulkan driver raw `2559541248`；Controller framebuffer 520×720，FIFO。API 建立時 enabledLayerCount=0。

`verified-controller/` 三次均觀察到 child exit 0、report complete=true、drain=complete、stderr 0 bytes。執行檔 SHA256 為 `80E0A16F379E8B8C764E696E0F27EA03CE274D03209B2F65F38A48B5A3F46C50`。

| Run | UI builds / 60 s | UI p50 / p95 | Present retired | Command buffers created | Fences created | Semaphores created |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| B1-01 | 47,211 | 0.0630 / 0.1998 ms | 8,504 | 8,504 | 25,512 | 17,008 |
| B1-02 | 47,441 | 0.0485 / 0.1363 ms | 8,636 | 8,636 | 25,908 | 17,272 |
| B1-03 | 47,777 | 0.0483 / 0.1352 ms | 8,636 | 8,636 | 25,908 | 17,272 |

三次 UI build 次數 median 為 47,441，range 為 47,211–47,777；UI build p95 的三次 median 為 0.1363 ms，range 為 0.1352–0.1998 ms。這組數值描述改動 cadence／同步物件重用前的 controller workload。Disabled/enabled 觀測成本以後述 B3／B4 配對量測記錄。

## B2–B6 基線與觀測成本

完整結果、環境指紋與逐 run manifests 位於 [replica baseline README](../../artifacts/vulkan-perf/task1-20260916-4eec1ed5/verified-replica/README.md)。批次於 2026-09-16 10:12:19–10:39:18 +08:00 串行執行，外層程序 exit 0。B2–B6 enabled 各三次，加上 B3／B4 disabled 各三次，共 21 runs；每份 fixture 都 complete=true、host／source exit 0、stderr 0 bytes。15 份 collector reports 都 complete=true、drain=complete，sample overflow 0。

受控 GDI 來源由獨立程序繪製；host 使用連結 production archives 的 local composition fixture，執行真實 `DesktopHost::run()`／Application／runtime。Formal shipping CLI 由既有 B1 與 CLI smokes 涵蓋，滑鼠／鍵盤的 UI 操作另有後續驗收。這組 fixture 的 source、executable 與 archives 共 15 筆 SHA256 於 10:38:26 再次核對相符；現行 production 來源指紋保持同版，測試差異單獨記錄。

| Workload | One-core CPU median | Presented FPS median | UI builds median / 60 s |
| --- | ---: | ---: | ---: |
| B2 static 1080p、Auto | 12.1005% | 0 | 47,258 |
| B3 dynamic 1080p、Auto | 12.6214% | 47.9166 | 49,457 |
| B4 dynamic 1080p、CPU | 17.9547% | 48.1335 | 49,464 |
| B5 dynamic 4K、CPU | 30.7506% | 50.3281 | 47,188 |
| B6 dynamic 4K、CPU、960×540 crop | 33.4025% | 50.2198 | 46,184 |

上述表格使用 enabled 模式。B3 實際走 gpu_interop；B4–B6 實際走 cpu_copy。B2 固定內容的穩態新 frame 數為 0，對應空白 latency 樣本與 `null` percentiles。B3–B6 最小化 controller 的 framebuffer 仍為 160×28，現行 host 持續建立 UI；這項證據支持 Task 2 同時依 iconified 與 framebuffer 判定可見性。

Enabled／disabled 三次 median 比較：B3 CPU 為 12.6214%／12.9332%，相對差 −2.4110%；三次成對差跨越正負，呈現現場波動。B4 CPU 為 17.9547%／15.5964%，增加 2.3582 百分點、相對 +15.1203%；三次 enabled 均高於配對 disabled。B3／B4 的 FPS median 相對差分別為 −0.0513%／−0.0181%。後續改善比較需沿用同種 instrumentation，並保留觀測成本與樣本範圍。

各 stage 的 byte counter 使用固定窗口內操作要求的資料量；stage 跨越窗口邊界時，累積值可相差一個 frame。B5／B6 的 3840×2160 allocation 維持每張 33,177,600 bytes；B6 的小 crop 同時保留完整 allocation 的 CPU clear／copy／upload 路徑，作為 row-packing 改善前對照。

## 接續 gate

1. 已完成：完整 spec／quality rolling review 達成 0 Critical／0 Medium。
2. 硬體擷取授權與 3 cases 已完成；同一組 fixture 的 B2–B6 共 21 runs、逐份 manifests 與 B3／B4 觀測成本紀錄已完成。
3. 既有 production 基線依階段完成至 `2f64894`，各自 V7 snapshot build 與全新 review 通過；詳細 tree、reviewer 與 build logs 見 [基線階段](2026-09-16-production-baseline-stages.md)。
4. Task 1 完成兩個 production commits：`a15baf6` collector、`10f0df9` observations／CLI／指南；獨立完整 frozen review 為 `task1_frozen_full_review_1`，0 Critical／0 Medium。兩個 snapshot 各自建置通過，root 再次核對 21 fixtures 與 15 complete reports。Task 2 UI cadence 接續進行。

Tests、spec、plan、verification 與 artifacts 皆保留工作樹。
