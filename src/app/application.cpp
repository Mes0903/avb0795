/// @file Main-thread use-case sequencing and the published-runtime ownership firewall.
#include "airelinna/app/application.hpp"

#include "airelinna/app/app_command_queue.hpp"
#include "airelinna/app/ports.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace airelinna::app {

  Application::Application(
    AppCommandQueue &commands, ISourceCatalog &source_catalog, IReplicaRuntimeFactory &runtime_factory, IGlobalHotkey &global_hotkey, ConfigStore &config_store, std::filesystem::path config_path, core::PlatformCapabilities capabilities, core::CaptureExclusionStatus controller_exclusion)
      : commands_{commands}, source_catalog_{source_catalog}, runtime_factory_{runtime_factory}, global_hotkey_{global_hotkey}, config_store_{config_store}, config_path_{std::move(config_path)}, capabilities_{capabilities}, controller_exclusion_{controller_exclusion}
  {
    initialize();
  }

  Application::~Application() = default;

  void Application::initialize()
  {
    const auto loaded = config_store_.load(config_path_);
    if (loaded) {
      config_ = *loaded;
    }
    else {
      config_ = UserConfig{};
      set_notice(NoticeOperation::ConfigRead, loaded.error());
    }

    // Only persisted preferences seed the session; mode starts Editable and no
    // source is auto-selected or captured during application initialization.
    const auto session_id = state_.current_session_id();
    (void) state_.set_fps(session_id, config_.fps_cap);
    (void) state_.set_border(session_id, config_.border);
    (void) state_.set_extensions(session_id, config_.extensions);

    refresh_sources();

    if (capabilities_.global_hotkey) {
      const auto registration = global_hotkey_.replace(config_.hotkey);
      if (registration) {
        hotkey_binding_confirmed_ = true;
        clear_notice(NoticeOperation::Hotkey);
      }
      else {
        hotkey_binding_confirmed_ = false;
        set_notice(NoticeOperation::Hotkey, registration.error());
      }
    }

    rebuild_view();
  }

  void Application::step(Clock::time_point now) noexcept
  {
    if (exit_disposition_ != core::ExitDisposition::Continue) {
      return;
    }

    try {
      // This call consumes the prior immediate request. Handlers may schedule a
      // new one when work is introduced after the runtime tick has already run.
      immediate_deadline_.reset();
      auto commands = commands_.drain();
      for (const auto &command : commands) {
        if (quit_intent_) {
          break;
        }
        process_command(command, now);
        if (exit_disposition_ == core::ExitDisposition::AbandonProcess) {
          return;
        }
      }

      // Runtime events are returned synchronously from the current owner, so no
      // separate callback event queue or stale-runtime epoch is needed in app.
      if (runtime_) {
        auto result = runtime_->tick(now);
        runtime_deadline_ = result.next_wake;
        process_runtime_result(std::move(result), now);
      }
      else {
        runtime_deadline_.reset();
      }

      if (exit_disposition_ != core::ExitDisposition::AbandonProcess) {
        rebuild_view();
      }
    } catch (...) {
      // Never unwind a published native aggregate because allocation, projection
      // or a non-noexcept service unexpectedly failed during an application step.
      emergency_abandon();
    }
  }

  const ControllerViewState &Application::view_state() const noexcept
  {
    return view_state_;
  }

  std::optional<Application::Clock::time_point>
  Application::next_deadline() const noexcept
  {
    // A stop/replacement requested after tick must override that old runtime's
    // empty or distant deadline, otherwise a headless drain could stop advancing.
    if (runtime_deadline_ && immediate_deadline_) {
      return (std::min) (*runtime_deadline_, *immediate_deadline_);
    }
    if (immediate_deadline_) {
      return immediate_deadline_;
    }
    return runtime_deadline_;
  }

  core::ExitDisposition Application::exit_disposition() const noexcept
  {
    return exit_disposition_;
  }

  void Application::shutdown() noexcept
  {
    begin_shutdown(Clock::now());
  }

  void Application::rebuild_view()
  {
    std::optional<core::AppError> notice;
    if (notice_) {
      notice = notice_->error;
    }
    view_state_ = build_controller_view(
      state_, state_.current_session_id(), sources_, capabilities_, config_,
      hotkey_binding_confirmed_, controller_exclusion_, runtime_feedback_,
      runtime_ != nullptr, stop_requested_, notice);
  }

  void Application::process_command(const AppCommand &command,
                                    Clock::time_point now)
  {
    std::visit(
      [this, now](const auto &value) { handle(value, now); }, command);
  }

  void Application::handle(const RefreshSources &, Clock::time_point)
  {
    const auto lifecycle = state_.current_session().lifecycle();
    // Catalog refresh may retire native source observations, so even an Error
    // runtime still draining must keep the current catalog generation alive.
    if (runtime_ || stop_requested_ ||
        (lifecycle != core::SessionLifecycle::Idle &&
         lifecycle != core::SessionLifecycle::Error)) {
      reject();
      return;
    }
    refresh_sources();
    clear_notice(NoticeOperation::Command);
  }

  void Application::handle(const SelectSource &command, Clock::time_point)
  {
    if (!validate_session(command.session_id) || !selection_allowed()) {
      reject();
      return;
    }

    const auto found = std::ranges::find(
      sources_, command.source_id, &core::SourceDescriptor::id);
    if (found == sources_.end()) {
      reject(core::AppError{core::AppErrorCode::SourceUnavailable});
      return;
    }
    if (!source_supported(found->kind)) {
      reject(core::AppError{core::AppErrorCode::InvalidCommand});
      return;
    }

    const auto selected = state_.select_source(command.session_id, *found);
    if (!selected) {
      reject(selected.error());
      return;
    }
    clear_notice(NoticeOperation::Command);
  }

  void Application::handle(const Start &command, Clock::time_point now)
  {
    if (!validate_session(command.session_id) || stop_requested_) {
      reject();
      return;
    }

    const auto &session = state_.current_session();
    if (!session.selected_source() ||
        !source_supported(session.selected_source()->kind)) {
      reject();
      return;
    }

    if (runtime_) {
      if (session.lifecycle() != core::SessionLifecycle::Interrupted ||
          pending_start_) {
        reject();
        return;
      }
      // Snapshot the requested replacement before stopping the disconnected
      // owner; its overlay and source lease stay alive until safe Stopped.
      pending_start_ = PendingStart{.source = *session.selected_source(),
                                    .settings = session.settings()};
      const auto stopping = state_.begin_stop(command.session_id);
      if (!stopping) {
        pending_start_.reset();
        reject(stopping.error());
        return;
      }
      stop_requested_ = true;
      runtime_->request_stop();
      clear_notice(NoticeOperation::Command);
      return;
    }

    const auto lifecycle = session.lifecycle();
    if (lifecycle != core::SessionLifecycle::Idle &&
        lifecycle != core::SessionLifecycle::Error) {
      reject();
      return;
    }
    start_current(now, false);
  }

  void Application::handle(const Stop &command, Clock::time_point now)
  {
    if (!validate_session(command.session_id)) {
      reject();
      return;
    }

    if (stop_requested_) {
      // A second Stop changes intent only: cancel queued replacement, keep drain.
      pending_start_.reset();
      clear_notice(NoticeOperation::Command);
      return;
    }
    if (!runtime_) {
      reject();
      return;
    }

    const auto lifecycle = state_.current_session().lifecycle();
    if (lifecycle != core::SessionLifecycle::Capturing &&
        lifecycle != core::SessionLifecycle::Interrupted &&
        lifecycle != core::SessionLifecycle::RecoveringGraphics) {
      reject();
      return;
    }

    pending_start_.reset();
    request_stop(false, now, false);
    clear_notice(NoticeOperation::Command);
  }

  void Application::handle(const SetMode &command, Clock::time_point now)
  {
    if (!validate_session(command.session_id) || !settings_allowed() ||
        (command.mode == core::OverlayMode::PassThrough &&
         !capabilities_.pass_through)) {
      reject();
      return;
    }

    auto candidate = state_.current_session().settings();
    candidate.mode = command.mode;
    if (!apply_settings(candidate, now)) {
      return;
    }
    const auto updated = state_.set_mode(command.session_id, command.mode);
    if (!updated) {
      reject(updated.error());
      return;
    }
    clear_notice(NoticeOperation::Command);
  }

  void Application::handle(const SetBorder &command, Clock::time_point now)
  {
    if (!validate_session(command.session_id) || !settings_allowed() ||
        (command.border == core::BorderPreference::Borderless &&
         !capabilities_.borderless_resize)) {
      reject();
      return;
    }

    auto candidate = state_.current_session().settings();
    candidate.border = command.border;
    if (!apply_settings(candidate, now)) {
      return;
    }
    const auto updated = state_.set_border(command.session_id, command.border);
    if (!updated) {
      reject(updated.error());
      return;
    }
    // Runtime/domain acceptance precedes persistence. Save failure must not undo
    // a native transaction that has already succeeded.
    config_.border = command.border;
    config_dirty_ = true;
    clear_notice(NoticeOperation::Command);
    persist_config();
  }

  void Application::handle(const SetFps &command, Clock::time_point now)
  {
    if (!validate_session(command.session_id) || !settings_allowed()) {
      reject();
      return;
    }

    auto candidate = state_.current_session().settings();
    candidate.fps_cap = command.fps_cap;
    if (!apply_settings(candidate, now)) {
      return;
    }
    const auto updated = state_.set_fps(command.session_id, command.fps_cap);
    if (!updated) {
      reject(updated.error());
      return;
    }
    config_.fps_cap = command.fps_cap;
    config_dirty_ = true;
    clear_notice(NoticeOperation::Command);
    persist_config();
  }

  void Application::handle(const SetExtensions &command, Clock::time_point now)
  {
    if (!validate_session(command.session_id) || !settings_allowed() ||
        !capabilities_.browser_capture_keep_alive) {
      reject();
      return;
    }

    auto candidate = state_.current_session().settings();
    candidate.extensions = command.extensions;
    if (!apply_settings(candidate, now)) {
      return;
    }
    const auto updated =
      state_.set_extensions(command.session_id, command.extensions);
    if (!updated) {
      reject(updated.error());
      return;
    }
    config_.extensions = command.extensions;
    config_dirty_ = true;
    clear_notice(NoticeOperation::Command);
    persist_config();
  }

  void Application::handle(const ReplaceHotkey &command, Clock::time_point)
  {
    if (!validate_session(command.session_id) || !settings_allowed() ||
        !capabilities_.global_hotkey) {
      reject();
      return;
    }

    // Desired text is not registration proof. Only successful replacement can
    // commit a new desired chord and confirm the corresponding OS binding.
    const auto replacement = global_hotkey_.replace(command.hotkey);
    if (!replacement) {
      // Conflict/cleanup failure preserve prior confirmation; explicit binding
      // loss clears it and later failures must not manufacture confirmation.
      if (replacement.error().code ==
          core::AppErrorCode::HotkeyRegistrationLost) {
        hotkey_binding_confirmed_ = false;
      }
      set_notice(NoticeOperation::Hotkey, replacement.error());
      return;
    }

    config_.hotkey = command.hotkey;
    hotkey_binding_confirmed_ = true;
    config_dirty_ = true;
    clear_notice(NoticeOperation::Hotkey);
    clear_notice(NoticeOperation::Command);
    persist_config();
  }

  void Application::handle(const RestoreActiveReplicaEditable &,
                           Clock::time_point now)
  {
    // Resolve the global hotkey against current state, not a captured SessionId.
    // A late hotkey while draining cannot apply settings to a retiring overlay.
    if (!runtime_ || stop_requested_) {
      return;
    }
    const auto lifecycle = state_.current_session().lifecycle();
    if (lifecycle != core::SessionLifecycle::Capturing &&
        lifecycle != core::SessionLifecycle::Interrupted &&
        lifecycle != core::SessionLifecycle::RecoveringGraphics) {
      return;
    }

    auto candidate = state_.current_session().settings();
    candidate.mode = core::OverlayMode::Editable;
    if (!apply_settings(candidate, now)) {
      return;
    }
    (void) state_.set_mode(state_.current_session_id(),
                           core::OverlayMode::Editable);
  }

  void Application::handle(const Quit &, Clock::time_point now)
  {
    begin_shutdown(now);
  }

  bool Application::validate_session(core::SessionId session_id) noexcept
  {
    const auto result = state_.validate_session(session_id);
    if (!result) {
      set_notice(NoticeOperation::Command, result.error());
      return false;
    }
    return true;
  }

  bool Application::source_supported(core::SourceKind kind) const noexcept
  {
    return kind == core::SourceKind::Display ? capabilities_.display_capture
                                             : capabilities_.window_capture;
  }

  bool Application::settings_allowed() const noexcept
  {
    if (stop_requested_) {
      return false;
    }
    const auto lifecycle = state_.current_session().lifecycle();
    if (!runtime_) {
      return lifecycle == core::SessionLifecycle::Idle ||
             lifecycle == core::SessionLifecycle::Error;
    }
    return lifecycle == core::SessionLifecycle::Capturing ||
           lifecycle == core::SessionLifecycle::Interrupted ||
           lifecycle == core::SessionLifecycle::RecoveringGraphics;
  }

  bool Application::selection_allowed() const noexcept
  {
    if (stop_requested_) {
      return false;
    }
    const auto lifecycle = state_.current_session().lifecycle();
    if (!runtime_) {
      return lifecycle == core::SessionLifecycle::Idle ||
             lifecycle == core::SessionLifecycle::Error;
    }
    return lifecycle == core::SessionLifecycle::Interrupted;
  }

  bool Application::apply_settings(const core::ReplicaSettings &candidate,
                                   Clock::time_point now) noexcept
  {
    if (!runtime_) {
      clear_notice(NoticeOperation::Settings);
      return true;
    }

    const auto applied = runtime_->apply(candidate);
    if (applied) {
      clear_notice(NoticeOperation::Settings);
      return true;
    }

    // Ordinary apply errors guarantee verified rollback, so the domain remains
    // unchanged. RuntimeStateLost removes that guarantee and requires safe drain.
    if (applied.error().code == core::AppErrorCode::RuntimeStateLost) {
      pending_start_.reset();
      (void) state_.mark_error(state_.current_session_id());
      request_stop(true, now, false);
    }
    else {
      set_notice(NoticeOperation::Settings, applied.error());
    }
    return false;
  }

  void Application::start_current(Clock::time_point now,
                                  bool after_runtime_tick)
  {
    const auto session_id = state_.current_session_id();
    const auto &session = state_.current_session();
    if (!session.selected_source() ||
        !source_supported(session.selected_source()->kind)) {
      reject();
      return;
    }

    const auto starting = state_.begin_start(session_id);
    if (!starting) {
      reject(starting.error());
      return;
    }
    clear_notice(NoticeOperation::Command);

    auto created =
      runtime_factory_.create(*session.selected_source(), session.settings());
    if (!created || !*created) {
      set_notice(NoticeOperation::Command,
                 created ? core::AppError{
                             core::AppErrorCode::RuntimeConstructionFailure}
                         : created.error());
      (void) state_.mark_error(session_id);
      if (after_runtime_tick) {
        request_immediate(now);
      }
      return;
    }

    // Publication transfers the complete aggregate. Thereafter only Stopped
    // permits reset, including failure of the immediately following domain edge.
    runtime_ = std::move(*created);
    reset_runtime_feedback();
    stop_requested_ = false;
    const auto capturing = state_.mark_capturing(session_id);
    if (!capturing) {
      (void) state_.mark_error(session_id);
      request_stop(true, now, after_runtime_tick);
      return;
    }
    if (after_runtime_tick) {
      request_immediate(now);
    }
  }

  void Application::request_stop(bool preserve_error, Clock::time_point now, bool after_runtime_tick) noexcept
  {
    // Preserve Error independently of the stop bit: the UI must distinguish an
    // idle failed start from terminal Error with native owners still draining.
    if (!runtime_ || stop_requested_) {
      return;
    }
    if (!preserve_error) {
      const auto stopping = state_.begin_stop(state_.current_session_id());
      if (!stopping) {
        reject(stopping.error());
        return;
      }
    }
    stop_requested_ = true;
    runtime_->request_stop();
    if (after_runtime_tick) {
      request_immediate(now);
    }
  }

  void Application::refresh_sources()
  {
    auto refreshed = source_catalog_.enumerate();
    if (!refreshed) {
      set_notice(NoticeOperation::SourceRefresh, refreshed.error());
      return;
    }

    const auto &selected = state_.current_session().selected_source();
    if (selected) {
      const auto found = std::ranges::find(
        *refreshed, selected->id, &core::SourceDescriptor::id);
      if (found == refreshed->end()) {
        (void) state_.clear_source(state_.current_session_id());
      }
      else {
        (void) state_.select_source(state_.current_session_id(), *found);
      }
    }
    // Publish only a successful refresh; an enumeration error keeps the previous
    // portable snapshot, while missing IDs in a successful refresh clear selection.
    sources_ = std::move(*refreshed);
    clear_notice(NoticeOperation::SourceRefresh);
  }

  void Application::persist_config()
  {
    // Called by accepted config-backed commands. A later accepted command retries
    // a failed save; shutdown intentionally does not perform this synchronous I/O.
    const auto saved = config_store_.save(config_path_, config_);
    if (saved) {
      config_dirty_ = false;
      clear_notice(NoticeOperation::ConfigSave);
    }
    else {
      set_notice(NoticeOperation::ConfigSave, saved.error());
    }
  }

  void Application::process_runtime_result(RuntimeTickResult result,
                                           Clock::time_point now)
  {
    std::optional<core::AppError> failure;
    bool overlay_close{};
    bool source_lost{};
    bool recovery_started{};
    bool recovered{};
    bool stopped{};

    // First collect lifecycle candidates and apply every status edge. Runtime
    // slot order must not decide which portable lifecycle transition wins.
    for (const auto &event : result.events) {
      if (!event) {
        continue;
      }
      if (const auto *bridge = std::get_if<BridgeModeChanged>(&*event)) {
        runtime_feedback_.bridge = BridgeFeedback{.mode = bridge->mode,
                                                  .reason = bridge->reason};
      }
      else if (const auto *exclusion =
                 std::get_if<OverlayCaptureExclusionChanged>(&*event)) {
        runtime_feedback_.overlay_exclusion = exclusion->status;
      }
      else if (const auto *drain =
                 std::get_if<RuntimeDrainStatusChanged>(&*event)) {
        runtime_feedback_.drain = drain->status;
      }
      else if (std::holds_alternative<OverlayCloseRequested>(*event)) {
        overlay_close = true;
      }
      else if (std::holds_alternative<SourceLost>(*event)) {
        source_lost = true;
      }
      else if (std::holds_alternative<GraphicsRecoveryStarted>(*event)) {
        recovery_started = true;
      }
      else if (std::holds_alternative<GraphicsRecovered>(*event)) {
        recovered = true;
      }
      else if (std::holds_alternative<Stopped>(*event)) {
        stopped = true;
      }
      else if (const auto *failed = std::get_if<Failed>(&*event)) {
        failure = failed->error;
      }
    }

    if (quit_intent_ &&
        runtime_feedback_.drain == core::RuntimeDrainStatus::Stalled) {
      exit_disposition_ = core::ExitDisposition::AbandonProcess;
      return;
    }

    // Once stop has begun, late source/recovery/failure candidates cannot change
    // visible state. Stopped alone proves that resetting the owner is safe.
    if (stop_requested_) {
      if (stopped) {
        handle_stopped(now);
      }
      return;
    }

    if (stopped) {
      handle_unexpected_stopped();
      return;
    }

    // Protocol checks precede normal arbitration. Valid pre-stop candidates use
    // Failed > OverlayCloseRequested > SourceLost > causal recovery outcome.
    if (failure) {
      if (failure->code != core::AppErrorCode::RuntimeStateLost) {
        emergency_abandon();
        return;
      }
      pending_start_.reset();
      (void) state_.mark_error(state_.current_session_id());
      request_stop(true, now, true);
      return;
    }
    if (overlay_close) {
      pending_start_.reset();
      request_stop(false, now, true);
      return;
    }
    if (source_lost) {
      (void) state_.mark_interrupted(state_.current_session_id());
      return;
    }
    // Both facts can occur in one tick; preserve their causal order instead of
    // ranking completion below start and leaving the session stuck recovering.
    if (recovery_started) {
      (void) state_.begin_graphics_recovery(state_.current_session_id());
    }
    if (recovered) {
      (void) state_.complete_graphics_recovery(state_.current_session_id());
    }
  }

  void Application::handle_stopped(Clock::time_point now)
  {
    // The event certifies an empty aggregate. Release it before constructing a
    // replacement; no two published native runtimes coexist through this path.
    runtime_.reset();
    runtime_deadline_.reset();
    stop_requested_ = false;
    reset_runtime_feedback();

    if (quit_intent_) {
      exit_disposition_ = core::ExitDisposition::Clean;
      return;
    }

    if (pending_start_) {
      auto pending = std::move(*pending_start_);
      pending_start_.reset();
      core::ReplicaSession replacement;
      replacement.select_source(std::move(pending.source));
      replacement.set_mode(pending.settings.mode);
      replacement.set_border(pending.settings.border);
      replacement.set_fps(pending.settings.fps_cap);
      replacement.set_extensions(pending.settings.extensions);
      // Generation replacement invalidates commands queued from the old view.
      const auto replaced = state_.replace_session(std::move(replacement));
      if (!replaced) {
        (void) state_.complete_stop(state_.current_session_id());
        reject(replaced.error());
        return;
      }
      start_current(now, true);
      return;
    }

    // Terminal failure remains visible after its owners have safely drained.
    if (state_.current_session().lifecycle() ==
        core::SessionLifecycle::Error) {
      return;
    }
    const auto completed = state_.complete_stop(state_.current_session_id());
    if (!completed) {
      reject(completed.error());
    }
  }

  void Application::handle_unexpected_stopped() noexcept
  {
    // Stopped still proves empty ownership, but unsolicited completion violates
    // the stop protocol and cannot be reported to the user as an ordinary stop.
    runtime_.reset();
    runtime_deadline_.reset();
    stop_requested_ = false;
    pending_start_.reset();
    reset_runtime_feedback();
    (void) state_.mark_error(state_.current_session_id());
  }

  void Application::reset_runtime_feedback() noexcept
  {
    runtime_feedback_ = RuntimeFeedback{};
  }

  void Application::begin_shutdown(Clock::time_point now) noexcept
  {
    if (quit_intent_ ||
        exit_disposition_ != core::ExitDisposition::Continue) {
      return;
    }

    quit_intent_ = true;
    pending_start_.reset();
    commands_.close();
    global_hotkey_.clear();
    hotkey_binding_confirmed_ = false;
    view_state_.hotkey_binding_confirmed = false;

    // Preserve the unsaved-settings diagnostic without delaying native stop on
    // disk I/O. Only earlier command-time saves may have cleared the dirty bit.
    if (config_dirty_) {
      const auto error =
        core::AppError{core::AppErrorCode::ConfigWriteFailure};
      set_notice(NoticeOperation::Shutdown, error);
      view_state_.application_notice_text = error.user_message();
    }

    request_immediate(now);

    if (!runtime_) {
      exit_disposition_ = core::ExitDisposition::Clean;
      return;
    }

    if (!stop_requested_) {
      const auto lifecycle = state_.current_session().lifecycle();
      if (lifecycle == core::SessionLifecycle::Capturing ||
          lifecycle == core::SessionLifecycle::Interrupted ||
          lifecycle == core::SessionLifecycle::RecoveringGraphics) {
        (void) state_.begin_stop(state_.current_session_id());
      }
      stop_requested_ = true;
      runtime_->request_stop();
    }

    // Stalled may have arrived before Quit, so do not wait for a duplicate edge.
    // The retained owner must survive until the composition root's _Exit path.
    if (runtime_feedback_.drain == core::RuntimeDrainStatus::Stalled) {
      exit_disposition_ = core::ExitDisposition::AbandonProcess;
    }
  }

  void Application::emergency_abandon() noexcept
  {
    // Use fixed portable diagnostics and retain runtime_ even after stop is
    // requested. Returning AbandonProcess is not permission for stack unwinding.
    commands_.close();
    global_hotkey_.clear();
    hotkey_binding_confirmed_ = false;
    quit_intent_ = true;
    pending_start_.reset();
    if (runtime_ && !stop_requested_) {
      stop_requested_ = true;
      runtime_->request_stop();
    }
    set_notice(NoticeOperation::Emergency,
               core::AppError{core::AppErrorCode::PlatformOperationFailure});
    view_state_.hotkey_binding_confirmed = false;
    view_state_.application_notice_text =
      core::AppError{core::AppErrorCode::PlatformOperationFailure}
        .user_message();
    exit_disposition_ = core::ExitDisposition::AbandonProcess;
  }

  void Application::reject(core::AppError error) noexcept
  {
    set_notice(NoticeOperation::Command, error);
  }

  void Application::set_notice(NoticeOperation operation,
                               core::AppError error) noexcept
  {
    notice_ = PendingNotice{.operation = operation, .error = error};
  }

  void Application::clear_notice(NoticeOperation operation) noexcept
  {
    // A successful unrelated operation must not erase the most recent failure.
    if (notice_ && notice_->operation == operation) {
      notice_.reset();
    }
  }

  void Application::request_immediate(Clock::time_point now) noexcept
  {
    if (!immediate_deadline_ || now < *immediate_deadline_) {
      immediate_deadline_ = now;
    }
  }

}    // namespace airelinna::app
