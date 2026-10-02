# 透明度與選區驗證紀錄

## 開發基準

- 日期：2026-09-15。
- 專案：`D:/document/MesRepo/Airelinna`，產品 `avb0795`。
- Branch：`p4-application-release`；開發前 HEAD：`a955d2e`。
- 開發前檔案基準：`D:/document/MesRepo/5568ke/build/avb-opacity-region-baseline-20260915`，180 個檔案。
- 既有 dirty rename、icon、UX、文件保持原狀；index 初始為空。
- 使用 Visual Studio 18 Community x64 + 內附 stable CMake，`build/p4-release`。
- 基準 `avb0795`、`avb0795_core_tests`、`avb0795_windows_policy_tests` 建置成功。
- 基準非硬體 CTest：12/12 通過，0 失敗。

## 規格與計畫 review

- Spec round 1：0 Critical / 1 Medium，修正同 SessionId/同尺寸來源的 SourceId 綁定。
- Spec round 2：0 Critical / 1 Medium，修正 Interrupted retained runtime 與新 selected source 的 admission。
- Spec round 3：0 Critical / 0 Medium。
- Plan round 1：0 Critical / 4 Medium，修正 redraw admission、首次 presentation 初始化、native aspect 同步與 full-view 呈現後 armed input。
- Plan round 2：0 Critical / 0 Medium。
- 每輪皆使用全新 reviewer，主 agent 對照 live code 確認問題後修正。

## 功能驗證追蹤

下列結果來自 2026-09-15 的實際建置與測試。

- [x] Opacity config legacy/local-default、preview/commit、stale/invalid command。
- [x] Native opacity/mode/border round-trip 與可驗證 rollback。
- [x] 首次 Start 與 Stop→Start presentation 初始化。
- [x] Crop intersection、letterbox/reverse drag、UV texel-center 邊界。
- [x] Retained redraw 經 FPS admission 合併。
- [x] Full-view 成功呈現後 armed drag；舊 present late retirement 維持 geometry 失效。
- [x] Selection commit/cancel/reset、SourceId/extent 改變、lifecycle precedence。
- [x] Controller gesture command emission 與數值草稿。
- [x] 完整 Release build、非硬體 CTest、boundary/inventory checks。
- [x] 自有來源硬體驗證與實際可用 bridge 記錄。
- [x] 新功能整體最終 rolling review。

## 自動化結果與實際涵蓋

| 檢查 | 最終結果 | 涵蓋 |
| --- | --- | --- |
| Release build | exit 0 | `avb0795`、core tests、Windows policy tests、Windows graphics hardware tests |
| 非硬體 CTest | 12/12，0 failures | Application、UI、config、數學、排程、native policy 與既有契約 |
| Source boundaries | exit 0 | 編譯單元的分層依賴規則 |
| Test inventory | exit 0；15 cases | 沿用既有案例編號；hardware 為 13–15 |
| Graphics hardware | 2/2，exit 0 | 本次執行 cases 14、15，擷取測試自己建立的子視窗 |
| GPU 路徑另行核對 | case 15 exit 0 | `final_selection->mode == GpuInterop`，`gpu_interop_frames_submitted() == 1` |

Case 14 採 CPU copy，直接讀回 layered alpha、input mode，送入 native selection 手勢並檢查 typed result；套用結果後驗證退休 presentation 的 crop。來源 resize 使用該自有 HWND 的 `GraphicsCaptureItem.Size` 作為目標尺寸，並在新 capture pool 建立後要求子視窗繪出一張受控 frame。

來源關閉後的測試先等待中斷與清理，再清除 crop 並改成 25% 不透明度，確認 retained image 呈現完整來源，後續 idle tick 回到空 wake deadline。這同時覆蓋來源停止後的設定接受與重繪生命週期。

Case 15 使用實際 capability 所選 bridge；本機已確認為 GPU interop。靜態來源合併兩筆 crop 更新後，呈現計數增加一筆，capture submit 計數維持原值。完整框選、zero framebuffer 失效、舊 fence 晚退休與恢復後的新 snapshot 也透過具體 assertion 驗證。

硬體案例中的輪詢次數會隨排程改變，assertion 總數因此屬該次執行記錄。Case 13 屬另外的硬體驗收範圍，本次執行記錄為非硬體 12 cases 與 graphics hardware 2 cases。

## 實作 rolling review 與查證

- Opacity review：全新 reviewer，0 Critical / 0 Medium。
- Renderer review：1 Medium。新 capture 已接納時仍准許舊 retained image 重播；排程重現呈現紅燈，修正為同次 admission 由新 capture 或 retained redraw 消費後轉綠。
- Integration review：1 Medium。Interrupted 期間改選來源所需的 crop 清除被視為新增 crop 拒絕，阻擋通用偏好套用。自有來源原生回歸先呈現拒絕，縮小 runtime admission 限制至正值 crop 編輯後通過。
- Final round 1：1 Medium。來源停止後 dirty retained 工作受 capture job 條件阻擋。原生回歸確認畫面停留與持續 wake；將 capture intake 與 presentation admission 分開後通過。另同步 ConfigStore 文件中的 opacity 欄位。
- Final round 2：全新 reviewer 完整對照 prefeature baseline、各層所有權與文件，0 Critical / 0 Medium。
- Review 候選逐一核對 production 路徑；來源 refresh 的 stable-ID 假設與 fake recovery 事件組合經查證撤回。

各輪已完成的 reviewer 已結束工作。功能修改與既有 dirty baseline 分開核對；index 維持空白，提交整理方式待使用者選擇。Tests、spec、plan 保留工作樹。

## 產物與操作入口

- 執行檔：`build/p4-release/src/avb0795.exe`。
- 操作文件：[Replica 透明度與選區操作](../development/replica-controls.md)。
- 設定保存後續：[下一階段準備稿](../superpowers/specs/2026-09-15-config-persistence-preparation.md)。

## 人工操作項目

人工驗收狀態由實際操作者填入，與自動化測試結果分開保存。

- [ ] 拖曳與雙擊輸入 opacity；0% 經 controller 回復 100%。
- [ ] Borderless → PassThrough → Bordered → Editable，觀察 opacity 保留。
- [ ] 完整來源框選、反向拖曳、拖出 client、Esc、取消與 reset。
- [ ] 來源 resize、replica resize、跨 DPI 與切換焦點。
- [ ] XYWH 草稿與同尺寸來源切換。
