# Task 6：CPU content packing 與 padding clear

本 task 的 helper／renderer 工作與 Task 5B 的 DesktopHost 容量切換分開實作；Task 5 的 frozen acceptance／benchmark archives 維持原內容。Production 提交依 Task 5、Task 6 順序執行。

## Test-first 與 code review

Implementation agent `task6_cpu_rows_impl` 在既有 case 06 增加三個 SECTION：tight copy、padded/shrink、invalid-layout/overflow。首輪確認缺 API，再用空實作取得真正 assertion RED：55 failed／155 passed。完成實作後 policy build exit 0，cases 06／08 2/2，0.09 秒。Logs 位於 `artifacts/vulkan-perf/task6-20260916/policy-{red-test,green-build,green-test}.log`。

Root 逐項查核尺寸、containment、destination multiplication、source-last-row multiplication/addition、single-row SIZE_MAX pitch、所有檢查在寫入之前、disjoint storage 前置條件，以及 flush 的既有 ownership。純 helper 負責 byte layout；renderer 負責 mapped upload、noncoherent flush、計數與 CpuReady publication。Vulkan allocation-sized upload 維持原路徑。

全新 reviewer `task6_cpu_rows_review_1`：0 Critical／0 Medium。已獨立驗證 32／64-bit `size_t` 邊界、source Map borrow、copy=`C`／clear=`A-C`／upload=`A`、flush error 的 reservation discard 與 retained redraw。此 reviewer 為 read-only，未跑編譯／硬體；後續實測與文件 review 另記。

## Root verification

Task5B收尾的主工作樹full build／15-case CTest同時包含Task6 prepared code，全部通過（hardware3/3，4.07秒）。同一working build的isolatedvalidation cases14/15為2/2、VUID0，1.96秒；logs位於 `artifacts/vulkan-perf/task5b-20260916/`。Task5B snapshot／commit排除Task6，Task6本身的精確staged build、B4／B5／B6 byte checkpoints及包含新增文件的finalreview待後續。使用者要求完成Task5後停止，本輪Task6保持未提交。

## 2026-09-19 恢復與驗證

工作樹候選的 full build 成功，CTest 15/15，hardware 3/3，4.63 秒。Inventory 維持 15 cases。Isolated validation 1.4.357.0 的 cases 14/15 為 2/2、2.33 秒，loader log 確認實際 DLL，VUID／Validation Error 為 0。Logs 位於 `artifacts/vulkan-perf/task6-20260919/`。

Fresh spec reviewer `task6_spec_resume_20260919` 為 0 Critical／0 Medium，指出 development 指南的階段句仍稱 padding 為後續工作；root 查證後更新。接續全新 quality reviewer `task6_quality_resume_20260919` 為 0 Critical／0 Medium／0 Low。兩輪皆唯讀，實測由 root 執行。

六檔 staged tree `3a1775e2f996046852012f98897119d3ada5e8f2` 經精確 checkout-index snapshot、tests OFF 獨立 configure/build，avb0795、diagnostics、source boundary target 通過。Shipping executable SHA256 `337430506B4F2DB0CDDFBC17D853E897CEED0B50DAC89BF713F0D16015B712ED`，固定 fixture executable SHA256 `C4BEFE263424CD74DF96251879FFD3B8AE1FE0666EF28AFB85C722EA27E0483C`。

Windows 25H2 build 26200.9457、RTX 3070 Ti、driver 32.0.16.1062、Balanced。相較 9/16 的 OS build 26200.9445 已有更新；最終比較採當日新配對。固定 Task1 fixture source hash 保持 `C01D4D84531D89CF6D4CBE78C4A7F41CCE0220AF90220357A41A56444491D3D0`。Formal runs 移除 explicit validation env，各 10 秒 warmup＋60 秒 measurement，replica 960×540、controller minimized、CPU transport。

| Run | Artifact folder | Copy frames | Clear bytes | Upload frames | FPS | One-core CPU % | Intake p95 ms |
|---|---|---:|---:|---:|---:|---:|---:|
| B4 | B4 | 2826 | 0 | 2822 | 47.0363 | 7.1803 | 7.5541 |
| B5 | B5-retry | 2881 | 0 | 2881 | 48.0217 | 15.0745 | 7.2047 |
| B6 | B6-retry | 2850 | 0 | 2850 | 47.4924 | 16.3255 | 7.2270 |

B4 每幀 8,294,400 bytes；B5/B6 每幀 33,177,600 bytes。三者 content=allocation，因此 clear=0；copy 是 C 的整數倍，upload 精確等於新 frame submits×A，warmup 後 replica 六項 native create/destroy 均為 0。B6 crop 為 960×540，完整 4K transport 如設計。Copy 與 submit 在量測邊界可屬不同 pipeline 階段，counts 分別列出。

首次 B5 在約 42.6 秒結束，report 標記 metadata_changed／early_shutdown、exit20；原資料保留於 `B5/`，來源 producer 正常結束，具體外部幾何變更原因仍待現場確認。新的 B5-retry、B6-retry 均 exit0、complete=true、drain=complete、stderr 空白。以上是 Task6 correctness checkpoints；單次 p95，尤其 B4，保留為 Task7 fresh paired matrix 的定位輸入，整體效能驗收沿 Task7 gate 執行。
