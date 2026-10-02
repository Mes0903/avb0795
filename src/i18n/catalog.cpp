/// @file
/// @brief Compiled UTF-8 catalog rows with ordinal coverage checks and bounded lookup.
#include "avb/i18n/catalog.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace avb::i18n {
  namespace {

    // Every view borrows an entire literal, preserving both program lifetime and
    // trailing null termination for frontend APIs that consume C strings.
    struct Row {
      TextId id;
      std::string_view english;
      std::string_view traditional_chinese;
      std::string_view japanese;
    };

    constexpr std::array<Row, text_count> rows{{
      {TextId::UnknownText, "Unknown text", "未知文字", "不明なテキスト"},
      {TextId::ControllerTitle, "avb0795 Controller", "avb0795 控制器",
       "avb0795 コントローラー"},
      {TextId::GeneralSettings, "General settings", "一般設定", "一般設定"},
      {TextId::LanguageLabel, "Language", "語言", "言語"},
      {TextId::FollowSystem, "Follow system", "跟隨系統", "システムに従う"},
      {TextId::EffectiveLanguage, "Effective language", "目前語言",
       "現在の言語"},
      {TextId::LanguageEnglish, "English", "English", "English"},
      {TextId::LanguageTraditionalChinese, "繁體中文", "繁體中文", "繁體中文"},
      {TextId::LanguageJapanese, "日本語", "日本語", "日本語"},
      {TextId::LanguageChangesClosed,
       "Language changes are unavailable during shutdown.",
       "關閉期間無法變更語言。", "シャットダウン中は言語を変更できません。"},
      {TextId::StatusLabel, "Status", "狀態", "状態"},
      {TextId::SessionErrorLabel, "Session error", "工作階段錯誤",
       "セッションエラー"},
      {TextId::NoticeLabel, "Notice", "通知", "通知"},
      {TextId::ColorLabel, "Color", "色彩", "カラー"},
      {TextId::ControllerExclusionLabel, "Controller exclusion",
       "控制器擷取排除", "コントローラーのキャプチャ除外"},
      {TextId::OverlayExclusionLabel, "Overlay exclusion", "覆疊視窗擷取排除",
       "オーバーレイのキャプチャ除外"},
      {TextId::CleanupLabel, "Cleanup", "清理", "クリーンアップ"},
      {TextId::BridgeLabel, "Bridge", "橋接", "ブリッジ"},
      {TextId::Sources, "Sources", "來源", "ソース"},
      {TextId::Refresh, "Refresh", "重新整理", "更新"},
      {TextId::SourceType, "Type", "類型", "種類"},
      {TextId::SourceName, "Name", "名稱", "名前"},
      {TextId::AutoRefresh, "Auto-refresh (5 s)", "自動重新整理（每 5 秒）",
       "自動更新（5 秒）"},
      {TextId::AutoRefreshPaused, "Paused while a source is selected or active",
       "選取或使用來源期間暫停更新", "ソースの選択中・使用中は更新を一時停止"},
      {TextId::ChooseDisplay, "Display", "螢幕", "ディスプレイ"},
      {TextId::ChooseWindow, "Window", "視窗", "ウィンドウ"},
      {TextId::FreshSourcePicker, "Choose another source and start", "選取其他來源並開始", "ソースを選択して開始"},
      {TextId::AcquisitionPermission, "Waiting for source permission", "等待來源授權", "ソースの許可を待機中"},
      {TextId::AcquisitionPreparing, "Preparing capture stream", "正在準備擷取串流", "キャプチャストリームを準備中"},
      {TextId::AcquisitionCancelling, "Cancelling source selection", "正在取消來源選取", "ソースの選択をキャンセル中"},
      {TextId::CancelAcquisition, "Cancel", "取消", "キャンセル"},
      {TextId::Start, "Start", "開始", "開始"},
      {TextId::Stop, "Stop", "停止", "停止"},
      {TextId::Mode, "Mode", "模式", "モード"},
      {TextId::Editable, "Editable", "可編輯", "編集可能"},
      {TextId::PassThrough, "Pass-through", "穿透", "パススルー"},
      {TextId::Opacity, "Opacity", "不透明度", "不透明度"},
      {TextId::OpacityHelp,
       "Drag to adjust, or Ctrl+click to enter a percentage. "
       "0% is invisible; 100% is fully opaque. Applies to the entire replica "
       "window, including its border and title bar. Saved when editing ends.",
       "拖曳調整，或 Ctrl＋點擊輸入百分比。0% 完全不可見，100% 完全不透明。"
       "套用至整個複本視窗，包含邊框與標題列；編輯結束時儲存。",
       "ドラッグで調整するか、Ctrl＋クリックで割合を入力します。"
       "0% は非表示、100% は完全に不透明です。枠とタイトルバーを含むレプリカ"
       "ウィンドウ全体に適用し、編集の終了時に保存します。"},
      {TextId::WheelZoom, "Wheel zoom step", "滾輪縮放步幅", "ホイール拡大率"},
      {TextId::WheelZoomHelp,
       "Drag to adjust, or Ctrl+click to enter a percentage from 1% to 25%. "
       "The default is 5% per notch. In Editable mode, the wheel resizes the "
       "replica around its center; downward scrolling reverses the scale. "
       "Region selection keeps the current size. Saved when editing ends.",
       "拖曳調整，或 Ctrl＋點擊輸入 1%～25% 的百分比。預設每格 5%。"
       "可編輯模式下，滾輪以複本中心縮放視窗；向下滾動使用反向倍率。"
       "框選期間維持目前尺寸。編輯結束時儲存。",
       "ドラッグで調整するか、Ctrl＋クリックで 1%～25% の割合を入力します。"
       "既定値は 1 目盛りあたり 5% です。編集可能モードでは、ホイールで"
       "レプリカの中心を基準に拡大・縮小します。下方向は逆の倍率を使います。"
       "範囲選択中は現在のサイズを保ち、編集の終了時に保存します。"},
      {TextId::SourceRegion, "Source region", "來源選區", "ソース範囲"},
      {TextId::SourceContentSize, "Source pixels", "來源像素尺寸", "ソースのピクセルサイズ"},
      {TextId::SelectRegion, "Select region", "框選範圍", "範囲を選択"},
      {TextId::CancelRegion, "Cancel selection", "取消框選", "範囲選択をキャンセル"},
      {TextId::ResetRegion, "Restore full source", "還原全畫面", "全体表示に戻す"},
      {TextId::RegionHelp,
       "Choose Select region, then drag on the replica to select a region. "
       "Esc or Cancel restores the previous region. Restore full source shows the entire source.",
       "按下「框選範圍」，再於複本上拖曳選取範圍。Esc 或「取消框選」會恢復前次選區；"
       "「還原全畫面」會顯示完整來源。",
       "「範囲を選択」を押してから、レプリカ上でドラッグして範囲を選択します。"
       "Esc またはキャンセルで前の範囲に戻ります。「全体表示に戻す」でソース全体を表示します。"},
      {TextId::RegionAwaitingContent, "Available after the source is presented",
       "來源畫面顯示後即可操作", "ソースの表示後に操作できます"},
      {TextId::FrameRate, "Frame rate", "畫面更新率", "フレームレート"},
      {TextId::Fps30, "30 FPS", "30 FPS", "30 FPS"},
      {TextId::Fps60, "60 FPS", "60 FPS", "60 FPS"},
      {TextId::Fps144, "144 FPS", "144 FPS", "144 FPS"},
      {TextId::Unlimited, "Unlimited", "無限制", "無制限"},
      {TextId::Border, "Border", "邊框", "枠"},
      {TextId::Bordered, "Bordered", "有邊框", "枠あり"},
      {TextId::Borderless, "Borderless", "無邊框", "枠なし"},
      {TextId::Extensions, "Extensions", "擴充功能", "拡張機能"},
      {TextId::KeepAlive, "Browser capture keep-alive (experimental)",
       "瀏覽器擷取持續運作（實驗性）", "ブラウザーキャプチャの継続動作（試験的）"},
      {TextId::KeepAliveHelp,
       "Attempts to keep covered browser windows rendering during window "
       "capture. May also affect other browser windows and increase CPU/GPU "
       "usage and power consumption. Does not change browser settings. "
       "Minimized windows, inactive tabs, protected video, and locked or "
       "sleeping displays are not guaranteed. Browser recovery after "
       "disabling is best-effort. Experimental: browser updates may break "
       "this workaround.",
       "嘗試在視窗擷取期間，讓遭其他視窗遮住的瀏覽器視窗持續繪製。這也可能影響其他"
       "瀏覽器視窗，並增加 CPU/GPU 使用率與耗電量。不會變更瀏覽器設定。不保證適用於"
       "最小化的視窗、非使用中的分頁、受保護的影片，以及已鎖定或休眠的顯示器。停用後"
       "的瀏覽器復原僅為盡力處理。實驗性功能：瀏覽器更新可能使此因應措施失效。",
       "ウィンドウキャプチャ中、ほかのウィンドウに覆われたブラウザーウィンドウの描画"
       "を継続させようとします。ほかのブラウザーウィンドウにも影響し、CPU/GPU 使用率"
       "および消費電力が増加する場合があります。ブラウザーの設定は変更しません。最小"
       "化されたウィンドウ、非アクティブなタブ、保護された動画、ロック中またはスリー"
       "プ中のディスプレイでの動作は保証されません。無効化後のブラウザーの復旧はベス"
       "トエフォートです。試験的機能のため、ブラウザーの更新によってこの回避策が機能"
       "しなくなる場合があります。"},
      {TextId::ExtensionsScope, "Window captures only", "僅限視窗擷取",
       "ウィンドウキャプチャのみ"},
      {TextId::Hotkey, "Hotkey", "快捷鍵", "ホットキー"},
      {TextId::CurrentHotkey, "Current hotkey", "目前快捷鍵", "現在のホットキー"},
      {TextId::RecordHotkey, "Record hotkey", "錄製快捷鍵", "ホットキーを記録"},
      {TextId::RecordHotkeyPrompt,
       "Press A-Z, 0-9 or F1-F12 with optional Ctrl, Alt, Shift or Meta. "
       "Confirm to apply; Esc cancels.",
       "按下 A-Z、0-9 或 F1-F12，可搭配 Ctrl、Alt、Shift 或 Meta。按確認套用，Esc 取消。",
       "A-Z、0-9、F1-F12 を押してください。Ctrl、Alt、Shift、Meta を組み合わせられます。"
       "確認で適用、Esc でキャンセルします。"},
      {TextId::ReleaseRecordingKeys, "Release all keys, then record one chord.",
       "請先放開所有按鍵，再錄製一組快捷鍵。",
       "すべてのキーを離してから、組み合わせを記録してください。"},
      {TextId::ConfirmHotkey, "Confirm", "確認", "確認"},
      {TextId::CancelHotkey, "Cancel", "取消", "キャンセル"},
      {TextId::BindingLabel, "Binding", "繫結", "バインド"},
      {TextId::Confirmed, "confirmed", "已確認", "確認済み"},
      {TextId::Unconfirmed, "unconfirmed", "未確認", "未確認"},
      {TextId::Quit, "Quit", "結束", "終了"},
      {TextId::Minimize, "Minimize", "最小化", "最小化"},
      {TextId::ControlStopping, "The current replica is stopping.",
       "目前的複本正在停止。", "現在のレプリカを停止しています。"},
      {TextId::ControlLifecycle,
       "This action is unavailable in the current lifecycle.",
       "此動作無法在目前的生命週期中使用。",
       "この操作は現在のライフサイクルでは使用できません。"},
      {TextId::ControlCapability, "This capability is unavailable.",
       "這項功能無法使用。", "この機能は使用できません。"},
      {TextId::ControlMissingSource, "Select a source first.",
       "請先選取來源。", "先にソースを選択してください。"},
      {TextId::SourceDisplay, "display", "顯示器", "ディスプレイ"},
      {TextId::SourceWindow, "window", "視窗", "ウィンドウ"},
      {TextId::Idle, "Idle", "閒置", "アイドル"},
      {TextId::Starting, "Starting", "正在啟動", "開始中"},
      {TextId::Capturing, "Capturing", "正在擷取", "キャプチャ中"},
      {TextId::Stopping, "Stopping", "正在停止", "停止中"},
      {TextId::Interrupted, "Capture interrupted", "擷取已中斷",
       "キャプチャ中断"},
      {TextId::RecoveringGraphics, "Recovering graphics", "正在復原圖形系統",
       "グラフィックスを復旧中"},
      {TextId::Error, "Error", "錯誤", "エラー"},
      {TextId::SessionFailed, "Replica session failed.", "複本工作階段失敗。",
       "レプリカセッションに失敗しました。"},
      {TextId::BridgeGpuInterop, "GPU interop active.",
       "GPU 互通功能已啟用。", "GPU 相互運用が有効です。"},
      {TextId::BridgeCpuCopy, "CPU copy bridge active.",
       "CPU 複製橋接已啟用。", "CPU コピーブリッジが有効です。"},
      {TextId::BridgeAdapterMismatch,
       "CPU copy: graphics adapters could not be matched.",
       "CPU 複製：無法配對圖形配接器。",
       "CPU コピー：グラフィックスアダプターを照合できませんでした。"},
      {TextId::BridgeExternalImageUnsupported,
       "CPU copy: external images are unsupported.",
       "CPU 複製：不支援外部影像。",
       "CPU コピー：外部イメージはサポートされていません。"},
      {TextId::BridgeD3d11InterfacesUnavailable,
       "CPU copy: required D3D11 interfaces are unavailable.",
       "CPU 複製：必要的 D3D11 介面無法使用。",
       "CPU コピー：必要な D3D11 インターフェイスを使用できません。"},
      {TextId::BridgeSharedSyncUnavailable,
       "CPU copy: shared synchronization is unavailable.",
       "CPU 複製：共用同步無法使用。",
       "CPU コピー：共有同期を使用できません。"},
      {TextId::BridgeTimelineImportUnavailable,
       "CPU copy: timeline fence import is unavailable.",
       "CPU 複製：時間軸柵欄匯入無法使用。",
       "CPU コピー：タイムラインフェンスのインポートを使用できません。"},
      {TextId::BridgeWin32ImportUnavailable,
       "CPU copy: Win32 import procedures are unavailable.",
       "CPU 複製：Win32 匯入程序無法使用。",
       "CPU コピー：Win32 インポートプロシージャを使用できません。"},
      {TextId::BridgeMemoryTypeUnavailable,
       "CPU copy: no compatible memory type is available.",
       "CPU 複製：找不到相容的記憶體類型。",
       "CPU コピー：互換性のあるメモリタイプがありません。"},
      {TextId::BridgeInteropFailure,
       "CPU copy: a GPU interop operation failed.",
       "CPU 複製：GPU 互通操作失敗。",
       "CPU コピー：GPU 相互運用処理に失敗しました。"},
      {TextId::ColorHdrUnsupported,
       "HDR was detected but is unsupported.", "偵測到 HDR，但不支援。",
       "HDR が検出されましたが、サポートされていません。"},
      {TextId::ColorUnknownSdr,
       "Color detection is unavailable; SDR is assumed.",
       "無法偵測色彩；假設為 SDR。",
       "カラーを検出できないため、SDR とみなします。"},
      {TextId::ControllerExclusionUnsupported,
       "Controller capture exclusion is unsupported.",
       "不支援控制器擷取排除。",
       "コントローラーのキャプチャ除外はサポートされていません。"},
      {TextId::ControllerExclusionFailed,
       "Controller capture exclusion failed.", "控制器擷取排除失敗。",
       "コントローラーのキャプチャ除外に失敗しました。"},
      {TextId::OverlayExclusionUnsupported,
       "Overlay capture exclusion is unsupported.",
       "不支援覆疊視窗擷取排除。",
       "オーバーレイのキャプチャ除外はサポートされていません。"},
      {TextId::OverlayExclusionFailed,
       "Overlay capture exclusion failed.", "覆疊視窗擷取排除失敗。",
       "オーバーレイのキャプチャ除外に失敗しました。"},
      {TextId::DrainStalled, "Native resource cleanup has stalled.",
       "原生資源清理已停滯。", "ネイティブリソースのクリーンアップが進んでいません。"},
      {TextId::ErrorInvalidCommand,
       "That action is not available in the current state.",
       "目前狀態無法執行該動作。",
       "現在の状態ではその操作を実行できません。"},
      {TextId::ErrorInvalidValue, "A provided value is invalid.",
       "提供的值無效。", "指定された値は無効です。"},
      {TextId::ErrorConfigReadFailure,
       "The configuration could not be read.", "無法讀取設定。",
       "設定を読み込めませんでした。"},
      {TextId::ErrorConfigWriteFailure,
       "The configuration could not be saved.", "無法儲存設定。",
       "設定を保存できませんでした。"},
      {TextId::ErrorSourceCatalogFailure,
       "Capture sources could not be listed.", "無法列出擷取來源。",
       "キャプチャソースを一覧表示できませんでした。"},
      {TextId::ErrorPermissionDenied,
       "Permission to capture the source was denied.",
       "擷取來源的權限遭拒。", "ソースをキャプチャする権限が拒否されました。"},
      {TextId::ErrorSourceUnavailable,
       "The selected capture source is unavailable.",
       "選取的擷取來源無法使用。",
       "選択したキャプチャソースは利用できません。"},
      {TextId::ErrorHotkeyConflict,
       "The requested global hotkey is already in use.",
       "要求的全域快捷鍵已被使用。",
       "要求されたグローバルホットキーは既に使用されています。"},
      {TextId::ErrorHotkeyCleanupFailure,
       "The previous global hotkey could not be released.",
       "無法釋放先前的全域快捷鍵。",
       "以前のグローバルホットキーを解放できませんでした。"},
      {TextId::ErrorHotkeyRegistrationLost,
       "The global hotkey registration was lost.",
       "全域快捷鍵的註冊已遺失。", "グローバルホットキーの登録が失われました。"},
      {TextId::ErrorUnsupportedWindowClass,
       "The window configuration is unsupported.", "不支援此視窗設定。",
       "ウィンドウ構成はサポートされていません。"},
      {TextId::ErrorUnsupportedGraphics,
       "The graphics configuration is unsupported.", "不支援此圖形設定。",
       "グラフィックス構成はサポートされていません。"},
      {TextId::ErrorRuntimeConstructionFailure,
       "The replica could not be started.", "無法啟動複本。",
       "レプリカを開始できませんでした。"},
      {TextId::ErrorPlatformOperationFailure,
       "A platform operation failed.", "平台操作失敗。",
       "プラットフォーム操作に失敗しました。"},
      {TextId::ErrorRuntimeStateLost,
       "The active replica can no longer continue safely.",
       "使用中的複本已無法安全繼續執行。",
       "アクティブなレプリカは安全に動作を継続できません。"},
      {TextId::ErrorUnknown, "An unknown application error occurred.",
       "發生未知的應用程式錯誤。", "不明なアプリケーションエラーが発生しました。"},
    }};

    // Direct ordinal lookup stays safe to maintain only when additions preserve
    // order and supply all translations. Reject incomplete/misaligned rows at compile time.
    static_assert([] {
      for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto &row = rows[index];
        if (row.id != static_cast<TextId>(index) || row.english.empty() ||
            row.traditional_chinese.empty() || row.japanese.empty()) {
          return false;
        }
      }
      return true;
    }());

  }    // namespace

  std::string_view text(TextId id, Language language) noexcept
  {
    // Validate the key before choosing a language so even fabricated enum values
    // get a stable fallback without indexing beyond the immutable catalog.
    const auto index = static_cast<std::size_t>(id);
    if (index >= rows.size()) {
      return "Unknown text";
    }

    const auto &row = rows[index];
    switch (language) {
    case Language::English:
      return row.english;
    case Language::TraditionalChinese:
      return row.traditional_chinese;
    case Language::Japanese:
      return row.japanese;
    }
    return row.english;
  }

}    // namespace avb::i18n
