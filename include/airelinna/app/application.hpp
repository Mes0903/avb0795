/// @file Sole application orchestrator for commands, domain transitions and runtime ownership.
#ifndef AIRELINNA_APP_APPLICATION_HPP
#define AIRELINNA_APP_APPLICATION_HPP

#include "airelinna/app/commands.hpp"
#include "airelinna/app/config_store.hpp"
#include "airelinna/app/controller_view_state.hpp"
#include "airelinna/core/application_state.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace airelinna::app {

  class AppCommandQueue;
  class IGlobalHotkey;
  class IReplicaRuntime;
  class IReplicaRuntimeFactory;
  class ISourceCatalog;
  struct RuntimeTickResult;

  /// Main-thread use-case owner, including current runtime, settings and view snapshot.
  /// Services/queue are borrowed and must outlive this object. A retained runtime
  /// may be destroyed only after Stopped; the host must honor AbandonProcess by
  /// exiting without stack unwinding instead of destroying uncertain native owners.
  class Application final {
  public:
    /// Load preferences, enumerate sources, attempt initial hotkey registration and
    /// build the first snapshot. Construction may throw before a runtime is published.
    Application(AppCommandQueue &commands, ISourceCatalog &source_catalog, IReplicaRuntimeFactory &runtime_factory, IGlobalHotkey &global_hotkey, ConfigStore &config_store, std::filesystem::path config_path, core::PlatformCapabilities capabilities, core::CaptureExclusionStatus controller_exclusion);
    /// Ordinary destruction requires the host to have completed the safe shutdown path.
    ~Application();

    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;
    Application(Application &&) = delete;
    Application &operator=(Application &&) = delete;

    /// Drain one command batch, tick the retained runtime and rebuild the UI snapshot.
    /// Unexpected exceptions select AbandonProcess while retaining native ownership.
    void step(std::chrono::steady_clock::time_point now) noexcept;
    /// Borrow the latest snapshot; do not retain references into it across another step.
    [[nodiscard]] const ControllerViewState &view_state() const noexcept;
    /// Earliest runtime or application-requested wake for the host's next event wait.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    next_deadline() const noexcept;
    /// Read the host exit instruction independently of the visible session lifecycle.
    [[nodiscard]] core::ExitDisposition exit_disposition() const noexcept;
    /// Idempotently close command admission, clear hotkey and initiate runtime drain.
    /// Continue calling step until exit disposition changes; this operation never joins.
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
    void initialize();
    /// Publish a fresh projection from explicitly supplied portable facts.
    void rebuild_view();
    /// Dispatch one queued value to its use-case handler.
    void process_command(const AppCommand &command,
                         std::chrono::steady_clock::time_point now);
    /// Apply status edges first, then arbitrate one lifecycle outcome independently of slot order.
    void process_runtime_result(RuntimeTickResult result,
                                std::chrono::steady_clock::time_point now);

    void handle(const RefreshSources &command,
                std::chrono::steady_clock::time_point now);
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
    void handle(const ReplaceHotkey &command,
                std::chrono::steady_clock::time_point now);
    void handle(const RestoreActiveReplicaEditable &command,
                std::chrono::steady_clock::time_point now);
    void handle(const Quit &command,
                std::chrono::steady_clock::time_point now);

    [[nodiscard]] bool validate_session(core::SessionId session_id) noexcept;
    [[nodiscard]] bool source_supported(core::SourceKind kind) const noexcept;
    [[nodiscard]] bool settings_allowed() const noexcept;
    [[nodiscard]] bool selection_allowed() const noexcept;
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
    /// Save accepted preferences; only success clears the application's dirty bit.
    void persist_config();
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
    core::ApplicationState state_;
    std::vector<core::SourceDescriptor> sources_;
    UserConfig config_;
    core::PlatformCapabilities capabilities_;
    core::CaptureExclusionStatus controller_exclusion_;
    RuntimeFeedback runtime_feedback_;
    std::optional<PendingNotice> notice_;
    std::optional<PendingStart> pending_start_;
    std::optional<std::chrono::steady_clock::time_point> runtime_deadline_;
    std::optional<std::chrono::steady_clock::time_point> immediate_deadline_;
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

}    // namespace airelinna::app

#endif    // AIRELINNA_APP_APPLICATION_HPP
