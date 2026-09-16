/// @file
/// @brief Sole application orchestrator for commands, domain transitions and runtime ownership.
#ifndef AVB0795_APP_APPLICATION_HPP
#define AVB0795_APP_APPLICATION_HPP

#include "avb/app/commands.hpp"
#include "avb/app/config_store.hpp"
#include "avb/app/controller_view_state.hpp"
#include "avb/core/application_state.hpp"
#include "avb/i18n/language.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace avb::app {

  /// @brief Owned query invoked on the main thread when System needs resolution.
  /// Construction and accepted System commands trigger it; ordinary steps do not
  /// poll for OS changes. An empty or throwing query resolves safely to English.
  using SystemLanguageQuery = std::function<i18n::Language()>;

  class AppCommandQueue;
  class IGlobalHotkey;
  class IReplicaRuntime;
  class IReplicaRuntimeFactory;
  class ISourceCatalog;
  struct RuntimeTickResult;

  /// @brief Sequence user intentions, domain state and one exclusively owned runtime.
  ///
  /// Services/queue are borrowed and must outlive this object. A retained runtime
  /// may be destroyed only after Stopped; the host must honor AbandonProcess by
  /// exiting without stack unwinding instead of destroying uncertain native owners.
  /// Visible lifecycle and resource retention are separate: Error can mean either
  /// a failed start with no runtime or a failed runtime whose drain is incomplete.
  ///
  /// @pre Use and destroy on the main thread; injected services must not reenter
  /// application mutations. Call shutdown() and continue stepping to Clean before
  /// ordinary destruction when a runtime has been published.
  class Application final {
  public:
    /// @brief Load preferences and initialize discovery, hotkey and presentation state.
    /// The primary path and system query are owned values; services remain borrowed.
    /// legacy_config_path is borrowed only during initialization and never retained
    /// as a save destination. Typed
    /// service failures become notices while startup continues with available data.
    /// No capture is started. Unexpected exceptions may propagate during construction.
    Application(AppCommandQueue &commands, ISourceCatalog &source_catalog, IReplicaRuntimeFactory &runtime_factory, IGlobalHotkey &global_hotkey, ConfigStore &config_store, std::filesystem::path config_path, core::PlatformCapabilities capabilities, core::CaptureExclusionStatus controller_exclusion, SystemLanguageQuery system_language_query, const std::filesystem::path &legacy_config_path = {});
    /// @brief Destroy members without initiating or completing asynchronous shutdown.
    /// @pre No uncertain published runtime remains; AbandonProcess requires the
    /// host's non-unwinding exit path instead of this destructor.
    ~Application();

    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;
    Application(Application &&) = delete;
    Application &operator=(Application &&) = delete;

    /// @brief Process one FIFO batch, tick the retained runtime once, and publish a view.
    /// Quit discards the unprocessed remainder of that batch. A runtime constructed
    /// while consuming tick results starts work on a subsequent step, requested by
    /// an immediate deadline. Calls after an exit decision do nothing.
    /// Unexpected exceptions select AbandonProcess while retaining native ownership;
    /// earlier accepted commands are not rolled back as a batch.
    /// @pre now is from the steady clock used by runtime and host deadlines.
    void step(std::chrono::steady_clock::time_point now) noexcept;
    /// @brief Borrow the latest published projection without extending its lifetime.
    /// step() replaces it and shutdown() can update it in place. Do not retain
    /// references into owned rows/strings across either mutation or owner destruction;
    /// copied catalog string_views continue to refer to static storage.
    [[nodiscard]] const ControllerViewState &view_state() const noexcept;
    /// @brief Return the earliest runtime, discovery or immediate application deadline.
    /// An empty result means no timer is currently requested, not that no runtime
    /// exists or no future event can arrive. The host must also wake for queued input
    /// and native events, and check exit disposition before waiting.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    next_deadline() const noexcept;
    /// Read the host exit instruction independently of the visible session lifecycle.
    [[nodiscard]] core::ExitDisposition exit_disposition() const noexcept;
    /// @brief Close command admission, clear the hotkey and initiate retained-runtime drain.
    /// Idempotent while shutdown is underway. Continue calling step until exit
    /// disposition changes; this operation never joins or performs a final config save.
    /// Unsaved accepted settings remain a notice, and known stalled drain selects
    /// AbandonProcess immediately rather than granting destruction permission.
    void shutdown() noexcept;

  private:
    using Clock = std::chrono::steady_clock;

    /// Tags the single notice so success clears only a notice from the same operation.
    enum class NoticeOperation {
      Command,
      ConfigRead,
      SourceRefresh,
      Settings,
      Hotkey,
      ConfigSave,
      Shutdown,
      Emergency,
    };

    /// One portable application notice, separate from the session Error lifecycle.
    struct PendingNotice {
      NoticeOperation operation;
      core::AppError error;
    };

    /// Owned replacement selection/settings retained until the old runtime reports Stopped.
    struct PendingStart {
      core::SourceDescriptor source;
      core::ReplicaSettings settings;
    };

    /// Initialize defaults/services before the first controller frame; never auto-start capture.
    void initialize(const std::filesystem::path &legacy_config_path);
    /// Publish a fresh projection from explicitly supplied portable facts.
    void rebuild_view();
    /// Dispatch one queued value to its use-case handler.
    void process_command(const AppCommand &command,
                         std::chrono::steady_clock::time_point now);
    /// @brief Apply status edges before lifecycle arbitration, independent of slot order.
    /// Stop phase admits only Stopped. Before stop, unexpected Stopped is a protocol
    /// error; valid candidates prioritize failure, close, source loss, then recovery
    /// start/completion in causal order. Quit plus stalled drain selects abandonment.
    void process_runtime_result(RuntimeTickResult result,
                                std::chrono::steady_clock::time_point now);

    void handle(const RefreshSources &command,
                std::chrono::steady_clock::time_point now);
    void handle(const SetAutoRefresh &command, Clock::time_point now);
    void handle(const SelectSource &command,
                std::chrono::steady_clock::time_point now);
    void handle(const Start &command,
                std::chrono::steady_clock::time_point now);
    void handle(const Stop &command,
                std::chrono::steady_clock::time_point now);
    void handle(const SetMode &command,
                std::chrono::steady_clock::time_point now);
    void handle(const SetBorder &command,
                std::chrono::steady_clock::time_point now);
    void handle(const SetFps &command,
                std::chrono::steady_clock::time_point now);
    void handle(const SetExtensions &command,
                std::chrono::steady_clock::time_point now);
    void handle(const SetOpacity &command,
                std::chrono::steady_clock::time_point now);
    void handle(const SetRegion &command, Clock::time_point now);
    void handle(const SetRegionSelection &command, Clock::time_point now);
    void handle(const ReplaceHotkey &command,
                std::chrono::steady_clock::time_point now);
    void handle(const RestoreActiveReplicaEditable &command,
                std::chrono::steady_clock::time_point now);
    void handle(const SetLanguage &command,
                std::chrono::steady_clock::time_point now);
    void handle(const Quit &command,
                std::chrono::steady_clock::time_point now);

    [[nodiscard]] bool validate_session(core::SessionId session_id) noexcept;
    [[nodiscard]] bool source_supported(core::SourceKind kind) const noexcept;
    [[nodiscard]] bool settings_allowed() const noexcept;
    [[nodiscard]] bool selection_allowed() const noexcept;
    /// Admit source-local mutations only against this runtime's presented content.
    [[nodiscard]] bool region_allowed() const noexcept;
    [[nodiscard]] bool source_matches(const core::SourceId &source_id) const noexcept;
    /// Native lifecycle facts certify cancellation; forget their obsolete UI bounds.
    void invalidate_region_interaction() noexcept;
    /// Accept the candidate only after successful native apply; terminal loss begins drain.
    [[nodiscard]] bool apply_settings(const core::ReplicaSettings &candidate,
                                      std::chrono::steady_clock::time_point now) noexcept;
    /// Publish a fully constructed runtime and request another step if this tick already ran.
    void start_current(std::chrono::steady_clock::time_point now,
                       bool after_runtime_tick);
    /// Separate stop phase from visible state so terminal Error remains visible during drain.
    void request_stop(bool preserve_error,
                      std::chrono::steady_clock::time_point now,
                      bool after_runtime_tick) noexcept;
    /// Release the reported-empty runtime, then complete quit/replacement/error/ordinary stop.
    void handle_stopped(std::chrono::steady_clock::time_point now);
    /// Treat unsolicited Stopped as a protocol error after releasing the reported-empty owner.
    void handle_unexpected_stopped() noexcept;
    /// Reconcile selection against a successfully refreshed portable source snapshot.
    void refresh_sources();
    /// @brief Protect source generations while a selection or runtime retains them.
    /// Automatic discovery runs during empty idle/error browsing. Manual refresh
    /// remains an explicit action that can replace an idle selection's generation.
    [[nodiscard]] bool automatic_refresh_allowed() const noexcept;
    /// @brief Advance one five-second discovery interval after the command batch.
    /// Selection wins over a due timer in the same step. Pauses discard the old
    /// deadline; resuming grants a full interval and overdue ticks enumerate once.
    void update_source_refresh(Clock::time_point now);
    /// Save accepted preferences; only success clears the application's dirty bit.
    void persist_config();
    /// Resolve the effective presentation language through the injected system boundary.
    void refresh_language() noexcept;
    /// Remove feedback from the previous runtime without clearing process-scoped exclusion.
    void reset_runtime_feedback() noexcept;
    /// Initiate shutdown and evaluate already-known stalled drain without waiting for another edge.
    void begin_shutdown(std::chrono::steady_clock::time_point now) noexcept;
    /// Contain unexpected exceptions/protocol violations without releasing uncertain owners.
    void emergency_abandon() noexcept;
    void reject(core::AppError error =
                  core::AppError{core::AppErrorCode::InvalidCommand}) noexcept;
    void set_notice(NoticeOperation operation, core::AppError error) noexcept;
    void clear_notice(NoticeOperation operation) noexcept;
    /// Prevent post-tick stop/replacement work from sleeping on an obsolete runtime deadline.
    void request_immediate(std::chrono::steady_clock::time_point now) noexcept;

    // Borrowed composition dependencies; their concrete native implementations stay outside app.
    AppCommandQueue &commands_;
    ISourceCatalog &source_catalog_;
    IReplicaRuntimeFactory &runtime_factory_;
    IGlobalHotkey &global_hotkey_;
    ConfigStore &config_store_;
    std::filesystem::path config_path_;
    SystemLanguageQuery system_language_query_;
    core::ApplicationState state_;
    std::vector<core::SourceDescriptor> sources_;
    UserConfig config_;
    i18n::Language effective_language_{i18n::Language::English};
    core::PlatformCapabilities capabilities_;
    core::CaptureExclusionStatus controller_exclusion_;
    RuntimeFeedback runtime_feedback_;
    std::optional<PendingNotice> notice_;
    std::optional<PendingStart> pending_start_;
    std::optional<std::chrono::steady_clock::time_point> runtime_deadline_;
    std::optional<std::chrono::steady_clock::time_point> immediate_deadline_;
    std::optional<Clock::time_point> source_refresh_deadline_;
    ControllerViewState view_state_;
    core::ExitDisposition exit_disposition_{core::ExitDisposition::Continue};
    // Accepted values remain authoritative after a failed command-time save.
    bool config_dirty_{};
    // True means the OS binding is exactly the current in-memory desired chord.
    bool hotkey_binding_confirmed_{};
    bool quit_intent_{};
    // Resource stop phase is orthogonal to Error versus Stopping in the domain.
    bool stop_requested_{};
    // Sole published-runtime owner. A status warning or stop request cannot reset it.
    std::unique_ptr<IReplicaRuntime> runtime_;
  };

}    // namespace avb::app

#endif    // AVB0795_APP_APPLICATION_HPP
