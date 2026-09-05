/// @file
/// @brief Main-thread replica aggregate, native progress, and ordered safe teardown.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/windows_replica_runtime.hpp"

#include "platform/windows/overlay_window.hpp"
#include "platform/windows/wgc_capture_job.hpp"
#include "platform/windows/win32_overlay_adapter.hpp"
#include "platform/windows/windows_capture_source.hpp"

#include <dxgiformat.h>
#include <winerror.h>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace airelinna::platform::windows {
  namespace {

    using Clock = std::chrono::steady_clock;

    [[nodiscard]] constexpr core::AppError runtime_state_error(
      const std::optional<std::int64_t> native_detail = std::nullopt) noexcept
    {
      return {.code = core::AppErrorCode::RuntimeStateLost,
              .native_detail = native_detail};
    }

    void earlier_deadline(
      std::optional<Clock::time_point> &current,
      const std::optional<Clock::time_point> candidate) noexcept
    {
      if (candidate && (!current || *candidate < *current)) {
        current = candidate;
      }
    }

    [[nodiscard]] constexpr bool
    is_zero_extent(const core::Extent extent) noexcept
    {
      return extent.width == 0 || extent.height == 0;
    }

  }    // namespace

  // Collect native facts independently; the use-case orchestrator ranks lifecycle outcomes.
  struct WindowsReplicaRuntime::TickCandidates final {
    std::optional<render::BridgeSelection> bridge;
    std::optional<core::CaptureExclusionStatus> capture_exclusion;
    std::optional<core::RuntimeDrainStatus> drain_status;
    bool overlay_close{};
    bool source_lost{};
    bool graphics_recovery_started{};
    bool graphics_recovered{};
    std::optional<core::AppError> failed;
    bool stopped{};
  };

  // Bind concrete owners to the small transaction algorithm without exposing another port.
  struct WindowsReplicaRuntime::SettingsOperations final {
    using Snapshot = core::ReplicaSettings;

    WindowsReplicaRuntime &owner;
    core::ReplicaSettings requested;
    Clock::time_point now;
    std::optional<OverlayClientGeometry> geometry;
    bool border_changed{};

    [[nodiscard]] std::optional<Snapshot> snapshot() const noexcept
    {
      return owner.settings_;
    }

    [[nodiscard]] bool apply_mode()
    {
      const auto changed = owner.adapter_->set_mode(requested.mode);
      if (!changed &&
          changed.error().code == core::AppErrorCode::RuntimeStateLost) {
        throw changed.error();
      }
      return changed.has_value();
    }

    [[nodiscard]] bool apply_border()
    {
      border_changed = requested.border != owner.settings_.border;
      if (!border_changed) {
        return true;
      }
      auto captured = owner.adapter_->snapshot_client_geometry();
      if (!captured) {
        if (captured.error().code == core::AppErrorCode::RuntimeStateLost) {
          throw captured.error();
        }
        return false;
      }
      geometry = *captured;
      const auto changed = owner.overlay_->set_border(requested.border);
      if (!changed) {
        if (changed.error().code == core::AppErrorCode::RuntimeStateLost) {
          throw changed.error();
        }
        return false;
      }
      owner.adapter_->set_border_preference(requested.border);
      return true;
    }

    [[nodiscard]] bool apply_geometry()
    {
      if (!border_changed) {
        return true;
      }
      const auto restored = owner.adapter_->restore_client_geometry(*geometry);
      if (!restored &&
          restored.error().code == core::AppErrorCode::RuntimeStateLost) {
        throw restored.error();
      }
      return restored.has_value();
    }

    [[nodiscard]] bool verify_applied() const noexcept
    {
      return owner.adapter_->mode() == requested.mode &&
             owner.adapter_->border_preference() == requested.border &&
             owner.overlay_->border_matches(requested.border);
    }

    [[nodiscard]] bool rollback(const Snapshot &snapshot) noexcept
    {
      bool restored{true};
      const auto mode = owner.adapter_->set_mode(snapshot.mode);
      restored = restored && mode.has_value();
      const auto border = owner.overlay_->set_border(snapshot.border);
      restored = restored && border.has_value();
      owner.adapter_->set_border_preference(snapshot.border);
      if (geometry) {
        const auto position = owner.adapter_->restore_client_geometry(*geometry);
        restored = restored && position.has_value();
      }
      return restored;
    }

    [[nodiscard]] bool verify_rollback(const Snapshot &snapshot) const noexcept
    {
      const bool native_settings_match =
        owner.adapter_->mode() == snapshot.mode &&
        owner.adapter_->border_preference() == snapshot.border &&
        owner.overlay_->border_matches(snapshot.border);
      return native_settings_match &&
             (!geometry || owner.adapter_->client_geometry_matches(*geometry));
    }

    // FPS scheduling/in-memory settings change only after native settings have verified.
    void commit_scheduler() noexcept
    {
      owner.scheduler_ = core::FrameScheduler{requested.fps_cap, now};
      owner.settings_ = requested;
    }
  };

  WindowsReplicaRuntime::WindowsReplicaRuntime(
    ConstructionToken, std::unique_ptr<WindowsCaptureSource> source, std::unique_ptr<OverlayWindow> overlay, std::unique_ptr<Win32OverlayAdapter> adapter, std::unique_ptr<render::WindowsRenderStack> render_stack, std::unique_ptr<WgcCaptureJob> capture_job, const core::ReplicaSettings settings, const core::Extent allocation, const core::CaptureExclusionStatus capture_exclusion, const render::WindowsBridgePreference bridge_preference, const std::uint32_t pool_buffer_count, WakeMainLoop wake_main_loop) noexcept
      : source_{std::move(source)}, overlay_{std::move(overlay)}, adapter_{std::move(adapter)}, render_stack_{std::move(render_stack)}, capture_job_{std::move(capture_job)}, scheduler_{settings.fps_cap, Clock::now()}, settings_{settings}, allocation_{allocation}, bridge_preference_{bridge_preference}, pool_buffer_count_{pool_buffer_count}, wake_main_loop_{std::move(wake_main_loop)}, initial_capture_exclusion_{capture_exclusion} {}

  WindowsReplicaRuntime::~WindowsReplicaRuntime() noexcept
  {
    if (published_ && !stopped_) {
      std::terminate();
    }
  }

  std::optional<WindowsRuntimeDiagnostics>
  WindowsReplicaRuntime::diagnostics() const noexcept
  {
    if (!render_stack_) {
      return std::nullopt;
    }
    return WindowsRuntimeDiagnostics{
      .allocation = allocation_,
      .resource_generation = resource_generation_,
      .bridge_mode = render_stack_->active_bridge_mode(),
      .captured_frames = last_render_result_.captured_frames,
      .presented_frames = last_render_result_.presented_frames,
      .dropped_frames = last_render_result_.dropped_frames,
      .slots_in_flight = last_render_result_.slots_in_flight,
      .presentations_in_flight = last_render_result_.presentations_in_flight,
      .presentation_suspended = last_render_result_.presentation_suspended,
    };
  }

  std::expected<void, core::AppError>
  WindowsReplicaRuntime::activate_for_publication() noexcept
  {
    if (!capture_job_ || published_) {
      return std::unexpected{runtime_state_error()};
    }
    try {
      const auto started = capture_job_->start_for_publication();
      if (!started) {
        return std::unexpected{started.error()};
      }
      // No failure path remains: it is now safe to expose the borrowed host wake binding.
      source_->activate_runtime_wake();
      published_ = true;
      return {};
    } catch (const core::AppError &error) {
      return std::unexpected{error};
    } catch (const std::bad_alloc &) {
      return std::unexpected{runtime_state_error(E_OUTOFMEMORY)};
    } catch (...) {
      return std::unexpected{runtime_state_error(E_FAIL)};
    }
  }

  void WindowsReplicaRuntime::cleanup_unpublished_activation_failure() noexcept
  {
    if (published_) {
      std::terminate();
    }
    // The factory guard may have to destroy the HWND while the adapter wrapper
    // is still alive. Release native users of that HWND before guard unwind.
    capture_job_.reset();
    render_stack_.reset();
  }

  std::expected<void, core::AppError>
  WindowsReplicaRuntime::apply(const core::ReplicaSettings &settings) noexcept
  {
    if (stop_requested_ || stopped_ || !overlay_ || !adapter_) {
      return std::unexpected{
        core::AppError{.code = core::AppErrorCode::InvalidCommand}};
    }
    SettingsOperations operations{
      .owner = *this, .requested = settings, .now = Clock::now()};
    return detail::apply_settings_transaction(operations);
  }

  // Stop is initiation only; later ticks retain each owner until its users are proven safe.
  void WindowsReplicaRuntime::request_stop() noexcept
  {
    if (stop_requested_ || stopped_) {
      return;
    }
    stop_requested_ = true;
    reconfiguration_phase_ = detail::SourceReconfigurationPhase::Inactive;
    pending_allocation_.reset();
    if (source_) {
      source_->request_stop();
    }
    if (capture_job_) {
      capture_job_->request_stop();
    }
    source_job_stopping_ = capture_job_ != nullptr;
    if (render_stack_) {
      render_stack_->begin_drain();
    }
  }

  void WindowsReplicaRuntime::begin_source_job_stop() noexcept
  {
    reconfiguration_phase_ = detail::SourceReconfigurationPhase::Inactive;
    pending_allocation_.reset();
    if (capture_job_) {
      capture_job_->request_stop();
      source_job_stopping_ = true;
    }
  }

  // Keep source/overlay identity stable and track Vulkan loss separately from D3 removal.
  void WindowsReplicaRuntime::begin_device_recovery(
    const render::DeviceLossKind kind, const Clock::time_point now) noexcept
  {
    if (kind == render::DeviceLossKind::VulkanDeviceLost) {
      vulkan_device_lost_ = true;
    }
    else {
      d3_device_removed_ = true;
    }
    if (render_stack_) {
      render_stack_->begin_device_recovery_drain(kind);
    }
    if (capture_job_) {
      capture_job_->request_stop();
    }
    source_job_stopping_ = capture_job_ != nullptr;
    reconfiguration_phase_ = detail::SourceReconfigurationPhase::Inactive;
    pending_allocation_.reset();
    device_recovery_phase_ = DeviceRecoveryPhase::Draining;
    if (!device_recovery_started_at_) {
      device_recovery_started_at_ = now;
    }
  }

  void WindowsReplicaRuntime::progress_source_reconfiguration(
    TickCandidates &candidates, render::DrainProgress &progress)
  {
    if (reconfiguration_phase_ == detail::SourceReconfigurationPhase::Inactive ||
        !capture_job_ || !render_stack_ || !pending_allocation_) {
      return;
    }

    // Collapse immediately-ready barrier stages, but keep per-tick transition work bounded.
    for (int step{}; step < 4; ++step) {
      const auto action = detail::next_source_reconfiguration_action(
        {.phase = reconfiguration_phase_,
         .callbacks_quiescent = capture_job_->frame_callbacks_quiescent(),
         .render_idle = render_stack_->render_allocation_idle(),
         .source_open = source_ && !source_->is_closed(),
         .capture_failed = false});
      switch (action) {
      case detail::SourceReconfigurationAction::None:
      case detail::SourceReconfigurationAction::Wait:
        return;
        // No old callback can publish after quiescence; discard its final mailbox checkout.
      case detail::SourceReconfigurationAction::DiscardFinalFrame:
        static_cast<void>(capture_job_->take_latest());
        reconfiguration_phase_ =
          detail::SourceReconfigurationPhase::WaitingForRender;
        progress = render::DrainProgress::TrackedRetirement;
        continue;
      case detail::SourceReconfigurationAction::Prepare: {
        if (resource_generation_ == (std::numeric_limits<std::uint64_t>::max)()) {
          candidates.failed = runtime_state_error();
          pending_failed_event_ = candidates.failed;
          begin_source_job_stop();
          return;
        }
        ++resource_generation_;
        const auto slots = render_stack_->rebuild_capture_slots(
          resource_generation_, *pending_allocation_,
          DXGI_FORMAT_B8G8R8A8_UNORM);
        if (!slots) {
          candidates.failed = runtime_state_error(slots.error().native_detail);
          pending_failed_event_ = candidates.failed;
          begin_source_job_stop();
          return;
        }
        const auto prepared = capture_job_->prepare_reconfigure(
          *pending_allocation_, resource_generation_);
        if (!prepared) {
          candidates.failed = runtime_state_error(prepared.error().native_detail);
          pending_failed_event_ = candidates.failed;
          begin_source_job_stop();
          return;
        }
        reconfiguration_phase_ = detail::SourceReconfigurationPhase::Prepared;
        progress = render::DrainProgress::ProvenSafeRelease;
        continue;
      }
        // The policy rechecked source openness before this no-throw prepared-epoch commit.
      case detail::SourceReconfigurationAction::Commit:
        capture_job_->commit_reconfigure();
        allocation_ = *pending_allocation_;
        adapter_->update_source_extent(allocation_);
        pending_allocation_.reset();
        reconfiguration_phase_ = detail::SourceReconfigurationPhase::Inactive;
        progress = render::DrainProgress::ProvenSafeRelease;
        return;
      case detail::SourceReconfigurationAction::Cancel:
        if (source_ && source_->is_closed()) {
          source_disconnected_ = true;
          if (!source_lost_published_ && !stop_requested_) {
            candidates.source_lost = true;
            pending_source_lost_event_ = true;
          }
        }
        begin_source_job_stop();
        return;
      }
    }
  }

  void WindowsReplicaRuntime::attempt_device_rebuild(
    const Clock::time_point now, TickCandidates &candidates, render::DrainProgress &progress, std::optional<Clock::time_point> &deadline)
  {
    if (device_recovery_phase_ != DeviceRecoveryPhase::WaitingToRebuild ||
        stop_requested_ || source_disconnected_ || !source_ || !overlay_) {
      return;
    }
    if (!device_recovery_started_at_) {
      device_recovery_started_at_ = now;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - *device_recovery_started_at_);
    const render::GraphicsRetryPolicy policy;
    const auto decision =
      policy.evaluate({.category = render::GraphicsFailureCategory::Device,
                       .attempt = device_recovery_episode_.attempt,
                       .current_generation = resource_generation_,
                       .elapsed_no_progress = elapsed});
    if (decision.action != render::GraphicsRetryAction::DeviceRebuild ||
        !decision.candidate_generation || !decision.retry_after) {
      candidates.failed = runtime_state_error();
      pending_failed_event_ = candidates.failed;
      return;
    }
    if (*decision.retry_after > std::chrono::milliseconds::zero()) {
      earlier_deadline(deadline, now + *decision.retry_after);
      return;
    }

    device_recovery_episode_ = render::advance_graphics_recovery_episode(
      device_recovery_episode_,
      render::GraphicsRecoveryObservation::ConstructionAttempted);
    std::optional<core::AppError> failure;
    try {
      // Rebuild locally; failed partial owners never replace the stable runtime aggregate.
      auto replacement_render = std::make_unique<render::WindowsRenderStack>(
        *overlay_, *decision.candidate_generation, bridge_preference_);
      auto replacement_job = WgcCaptureJob::create_inactive(
        *source_, replacement_render->capture_device(),
        *decision.candidate_generation, pool_buffer_count_, wake_main_loop_);
      if (!replacement_job) {
        failure = replacement_job.error();
      }
      else {
        const auto allocation = (*replacement_job)->initial_allocation();
        const auto slots = replacement_render->rebuild_capture_slots(
          *decision.candidate_generation, allocation,
          DXGI_FORMAT_B8G8R8A8_UNORM);
        if (!slots) {
          failure = slots.error();
        }
        else {
          const auto started = (*replacement_job)->start_for_publication();
          if (!started) {
            failure = started.error();
          }
          else {
            render_stack_ = std::move(replacement_render);
            capture_job_ = std::move(*replacement_job);
            allocation_ = allocation;
            adapter_->update_source_extent(allocation_);
            last_render_result_ = {};
            resource_generation_ = *decision.candidate_generation;
            device_recovery_phase_ = DeviceRecoveryPhase::Inactive;
            source_job_stopping_ = false;
            vulkan_device_lost_ = false;
            d3_device_removed_ = false;
            // Construction alone does not reset the retry episode; a present must follow.
            replacement_awaiting_present_ = true;
            device_recovery_episode_ = render::advance_graphics_recovery_episode(
              device_recovery_episode_,
              render::GraphicsRecoveryObservation::ReplacementConstructed);
            recovery_event_state_ = detail::observe_runtime_recovery_events(
              recovery_event_state_, {.completed = true});
            candidates.graphics_recovery_started =
              recovery_event_state_.pending_started;
            candidates.graphics_recovered =
              recovery_event_state_.pending_completed;
            bridge_event_state_ = detail::observe_runtime_bridge_selection(
              bridge_event_state_, render_stack_->take_bridge_mode_change());
            candidates.bridge = bridge_event_state_.pending;
            return;
          }
        }
      }
    } catch (const core::AppError &error) {
      failure = error;
    } catch (const std::bad_alloc &) {
      failure = runtime_state_error(E_OUTOFMEMORY);
    } catch (...) {
      failure = runtime_state_error(E_FAIL);
    }

    if (failure && failure->code == core::AppErrorCode::SourceUnavailable) {
      source_disconnected_ = true;
      if (!source_lost_published_) {
        candidates.source_lost = true;
        pending_source_lost_event_ = true;
      }
      device_recovery_phase_ = DeviceRecoveryPhase::Inactive;
      return;
    }
    if (device_recovery_episode_.attempt >= 3) {
      candidates.failed =
        runtime_state_error(failure ? failure->native_detail : std::nullopt);
      pending_failed_event_ = candidates.failed;
      return;
    }
    const auto next =
      policy.evaluate({.category = render::GraphicsFailureCategory::Device,
                       .attempt = device_recovery_episode_.attempt,
                       .current_generation = resource_generation_,
                       .elapsed_no_progress = elapsed});
    if (next.retry_after) {
      earlier_deadline(
        deadline, now + render::strictly_future_retry_delay(*next.retry_after));
    }
    else {
      candidates.failed =
        runtime_state_error(failure ? failure->native_detail : std::nullopt);
      pending_failed_event_ = candidates.failed;
    }
  }

  void WindowsReplicaRuntime::progress_device_recovery(
    const Clock::time_point now, const render::RenderTickResult &render_result, TickCandidates &candidates, render::DrainProgress &progress, std::optional<Clock::time_point> &deadline)
  {
    if (device_recovery_phase_ == DeviceRecoveryPhase::Draining) {
      if (capture_job_ && capture_job_->cleanup_failure()) {
        candidates.failed =
          runtime_state_error(capture_job_->cleanup_failure()->native_detail);
        pending_failed_event_ = candidates.failed;
        return;
      }
      const bool job_safe = !capture_job_ || capture_job_->stop_quiescent();
      const bool render_safe = !render_stack_ || render_result.drain_complete;
      if (!job_safe || !render_safe) {
        return;
      }
      capture_job_.reset();
      render_stack_.reset();
      source_job_stopping_ = false;
      progress = render::DrainProgress::ProvenSafeRelease;
      if (source_disconnected_) {
        device_recovery_phase_ = DeviceRecoveryPhase::Inactive;
        return;
      }
      device_recovery_phase_ = DeviceRecoveryPhase::WaitingToRebuild;
    }
    if (device_recovery_phase_ == DeviceRecoveryPhase::WaitingToRebuild &&
        source_disconnected_) {
      // The old job/render pair is already proven released. With the stable
      // source lease now closed there is nothing left to rebuild or drain.
      device_recovery_phase_ = DeviceRecoveryPhase::Inactive;
      device_recovery_started_at_.reset();
      replacement_awaiting_present_ = false;
      vulkan_device_lost_ = false;
      d3_device_removed_ = false;
      progress = render::DrainProgress::ProvenSafeRelease;
      return;
    }
    attempt_device_rebuild(now, candidates, progress, deadline);
  }

  // Dependency order is job -> graphics -> adapter/window -> stable source wake lease.
  void WindowsReplicaRuntime::progress_stop(
    const render::RenderTickResult &render_result, TickCandidates &candidates, render::DrainProgress &progress) noexcept
  {
    if (!stop_requested_ || stopped_) {
      return;
    }

    // Job-internal safety and render-held frame/presentation retirement are separate proofs.
    const bool job_safe = !capture_job_ || capture_job_->stop_quiescent();
    const bool render_safe = !render_stack_ || render_result.drain_complete;
    if (capture_job_ && job_safe && render_safe) {
      capture_job_.reset();
      source_job_stopping_ = false;
      progress = render::DrainProgress::ProvenSafeRelease;
    }
    if (render_stack_ && !capture_job_ && render_safe) {
      render_stack_.reset();
      progress = render::DrainProgress::ProvenSafeRelease;
    }
    if (adapter_ && !render_stack_) {
      const auto detached = adapter_->detach();
      if (!detached) {
        overlay_->destroy();
        adapter_->complete_after_overlay_destroyed();
      }
      adapter_.reset();
      overlay_.reset();
      progress = render::DrainProgress::ProvenSafeRelease;
    }
    // Keep the borrowed wake target alive through any final admitted source callback.
    if (source_ && !overlay_ && source_->callbacks_quiescent()) {
      source_.reset();
      progress = render::DrainProgress::ProvenSafeRelease;
    }
    if (!source_ && !overlay_ && !adapter_ && !render_stack_ && !capture_job_) {
      stopped_ = true;
      candidates.stopped = true;
    }
  }

  bool WindowsReplicaRuntime::cleanup_pending(
    const bool render_cleanup_pending) const noexcept
  {
    if (stop_requested_) {
      return !stopped_;
    }
    return render_cleanup_pending || source_job_stopping_ ||
           reconfiguration_phase_ !=
             detail::SourceReconfigurationPhase::Inactive ||
           device_recovery_phase_ != DeviceRecoveryPhase::Inactive;
  }

  bool WindowsReplicaRuntime::cleanup_uncertain() const noexcept
  {
    return capture_job_ &&
           detail::capture_cleanup_is_uncertain(capture_job_->cleanup_failure());
  }

  void WindowsReplicaRuntime::add_runtime_events(
    app::RuntimeTickResult &result, TickCandidates &candidates) noexcept
  {
    if (candidates.stopped) {
      result.events = {};
      result.events[0] = app::RuntimeEvent{app::Stopped{}};
      return;
    }

    std::size_t index{};
    const auto append = [&]<class Event>(Event event) noexcept {
      if (index < result.events.size()) {
        result.events[index++] = app::RuntimeEvent{event};
      }
    };
    if (candidates.bridge) {
      append(app::BridgeModeChanged{.mode = candidates.bridge->mode,
                                    .reason = candidates.bridge->reason});
    }
    if (candidates.capture_exclusion) {
      append(app::OverlayCaptureExclusionChanged{
        .status = *candidates.capture_exclusion});
    }
    if (candidates.drain_status) {
      append(app::RuntimeDrainStatusChanged{.status = *candidates.drain_status});
    }
    if (stop_requested_) {
      return;
    }
    if (candidates.overlay_close) {
      append(app::OverlayCloseRequested{});
    }
    if (candidates.source_lost) {
      append(app::SourceLost{});
    }
    if (candidates.graphics_recovery_started) {
      append(app::GraphicsRecoveryStarted{});
    }
    if (candidates.graphics_recovered) {
      append(app::GraphicsRecovered{});
    }
    if (candidates.failed) {
      append(app::Failed{.error = *candidates.failed});
    }
  }

  app::RuntimeTickResult
  WindowsReplicaRuntime::tick(const Clock::time_point now) noexcept
  {
    app::RuntimeTickResult result;
    try {
      TickCandidates candidates;
      candidates.capture_exclusion = initial_capture_exclusion_;
      candidates.bridge = bridge_event_state_.pending;
      candidates.failed = pending_failed_event_;
      candidates.overlay_close = pending_overlay_close_event_;
      candidates.source_lost = pending_source_lost_event_;
      candidates.graphics_recovery_started =
        recovery_event_state_.pending_started;
      candidates.graphics_recovered = recovery_event_state_.pending_completed;
      std::optional<Clock::time_point> deadline;
      render::DrainProgress progress{render::DrainProgress::None};

      // Service callback-local state synchronously before scheduling new render intake.
      const bool source_closed_edge = source_ && source_->consume_closed_edge();
      CaptureSignalBatch capture_signals;
      if (capture_job_) {
        capture_job_->service_pending_arrival();
        capture_signals = capture_job_->drain_signals();
      }
      const auto capture_facts = detail::merge_runtime_capture_signals(
        source_closed_edge, capture_signals);

      if (!stop_requested_ && capture_facts.source_closed) {
        source_disconnected_ = true;
        begin_source_job_stop();
        if (!source_lost_published_) {
          candidates.source_lost = true;
          pending_source_lost_event_ = true;
        }
      }
      if (!stop_requested_ && capture_facts.capture_failed) {
        candidates.failed = runtime_state_error(
          capture_signals.fatal.native_detail == 0
            ? std::nullopt
            : std::optional{capture_signals.fatal.native_detail});
        pending_failed_event_ = candidates.failed;
        begin_source_job_stop();
      }
      if (!stop_requested_ && !source_disconnected_ &&
          !capture_facts.capture_failed && capture_facts.resize &&
          *capture_facts.resize != allocation_ &&
          reconfiguration_phase_ ==
            detail::SourceReconfigurationPhase::Inactive &&
          device_recovery_phase_ == DeviceRecoveryPhase::Inactive &&
          capture_job_) {
        const auto begun = capture_job_->begin_reconfigure();
        if (!begun) {
          candidates.failed = runtime_state_error(begun.error().native_detail);
          pending_failed_event_ = candidates.failed;
          begin_source_job_stop();
        }
        else {
          pending_allocation_ = *capture_facts.resize;
          reconfiguration_phase_ =
            detail::SourceReconfigurationPhase::WaitingForCallbacks;
        }
      }

      if (!stop_requested_ && !source_job_stopping_ &&
          device_recovery_phase_ == DeviceRecoveryPhase::Inactive &&
          reconfiguration_phase_ ==
            detail::SourceReconfigurationPhase::Inactive &&
          capture_job_ && render_stack_) {
        const bool has_frame = capture_job_->has_latest();
        const auto schedule = scheduler_.poll(now, has_frame, false);
        earlier_deadline(deadline, schedule.next_wake);
        if (schedule.render && has_frame && render_stack_->ready_for_frame()) {
          if (auto frame = capture_job_->take_latest()) {
            if (frame->resource_generation() == resource_generation_ &&
                frame->allocation_extent() == allocation_ &&
                !is_zero_extent(frame->content_size())) {
              render_stack_->submit(std::move(*frame));
            }
          }
        }
      }

      // Even without a new frame, advance pending copies, presentations, and retirement.
      render::RenderTickResult render_result{.drain_complete =
                                               render_stack_ == nullptr};
      if (render_stack_) {
        const auto framebuffer = overlay_->framebuffer_extent();
        render_stack_->resize(framebuffer);
        render_result = render_stack_->tick(framebuffer, now);
        last_render_result_ = render_result;
        earlier_deadline(deadline, render_result.next_deadline);
        if (render_result.made_progress) {
          progress = render::DrainProgress::TrackedRetirement;
        }
        if (const auto bridge = render_stack_->take_bridge_mode_change()) {
          bridge_event_state_ = detail::observe_runtime_bridge_selection(
            bridge_event_state_, bridge);
          candidates.bridge = bridge_event_state_.pending;
        }
        if (replacement_awaiting_present_ &&
            render_result.presented_frames != 0) {
          device_recovery_episode_ = render::advance_graphics_recovery_episode(
            device_recovery_episode_,
            render::GraphicsRecoveryObservation::SuccessfulPresentRetired);
          replacement_awaiting_present_ = false;
          device_recovery_started_at_.reset();
        }

        // Neither loss fact authorizes cleanup of resources owned by the other API.
        const bool new_vulkan_loss =
          render_result.vulkan_device_lost && !vulkan_device_lost_;
        const bool new_d3_removal =
          render_result.d3_device_removed && !d3_device_removed_;
        const bool new_device_episode =
          !stop_requested_ &&
          device_recovery_phase_ == DeviceRecoveryPhase::Inactive &&
          (new_vulkan_loss || new_d3_removal);
        if (!stop_requested_) {
          recovery_event_state_ = detail::observe_runtime_recovery_events(
            recovery_event_state_,
            {.started =
               render_result.surface_recovery_started || new_device_episode,
             .completed = render_result.surface_recovery_completed,
             .completion_superseded =
               device_recovery_phase_ != DeviceRecoveryPhase::Inactive ||
               new_device_episode});
          candidates.graphics_recovery_started =
            recovery_event_state_.pending_started;
          candidates.graphics_recovered = recovery_event_state_.pending_completed;
        }
        if (new_vulkan_loss) {
          if (stop_requested_) {
            vulkan_device_lost_ = true;
            render_stack_->begin_device_recovery_drain(
              render::DeviceLossKind::VulkanDeviceLost);
          }
          else {
            begin_device_recovery(render::DeviceLossKind::VulkanDeviceLost, now);
          }
        }
        if (new_d3_removal) {
          if (stop_requested_) {
            d3_device_removed_ = true;
            render_stack_->begin_device_recovery_drain(
              render::DeviceLossKind::D3DeviceRemoved);
          }
          else {
            begin_device_recovery(render::DeviceLossKind::D3DeviceRemoved, now);
          }
        }
        if (!stop_requested_ && render_result.terminal_error &&
            !render_result.vulkan_device_lost &&
            !render_result.d3_device_removed) {
          candidates.failed =
            runtime_state_error(render_result.terminal_error->native_detail);
          pending_failed_event_ = candidates.failed;
        }
      }

      if (!stop_requested_ && overlay_ && overlay_->close_requested() &&
          !overlay_close_published_) {
        candidates.overlay_close = true;
        pending_overlay_close_event_ = true;
      }

      if (!stop_requested_ &&
          device_recovery_phase_ == DeviceRecoveryPhase::Inactive) {
        progress_source_reconfiguration(candidates, progress);
      }
      if (!stop_requested_ &&
          device_recovery_phase_ != DeviceRecoveryPhase::Inactive) {
        progress_device_recovery(now, render_result, candidates, progress,
                                 deadline);
      }
      if (!stop_requested_ && source_job_stopping_ &&
          device_recovery_phase_ == DeviceRecoveryPhase::Inactive) {
        if (capture_job_ && capture_job_->cleanup_failure()) {
          candidates.failed =
            runtime_state_error(capture_job_->cleanup_failure()->native_detail);
          pending_failed_event_ = candidates.failed;
        }
        else if (capture_job_ && capture_job_->stop_quiescent() &&
                 (!render_stack_ || render_stack_->render_allocation_idle())) {
          capture_job_.reset();
          source_job_stopping_ = false;
          progress = render::DrainProgress::ProvenSafeRelease;
        }
      }
      if (stop_requested_) {
        progress_stop(render_result, candidates, progress);
      }

      // Include callback/native cleanup in the same bounded observation deadline as graphics.
      const auto drain = detail::advance_runtime_drain(
        drain_state_,
        cleanup_pending(render_stack_ && render_result.cleanup_pending),
        cleanup_uncertain(), progress, now);
      drain_state_ = drain.state;
      candidates.drain_status = drain.status_change;
      earlier_deadline(deadline, drain.next_deadline);
      result.next_wake = deadline;
      add_runtime_events(result, candidates);
      if (candidates.stopped) {
        bridge_event_state_.pending.reset();
        pending_failed_event_.reset();
        pending_overlay_close_event_ = false;
        pending_source_lost_event_ = false;
        recovery_event_state_.pending_started = false;
        recovery_event_state_.pending_completed = false;
        initial_capture_exclusion_.reset();
      }
      else {
        if (candidates.bridge) {
          bridge_event_state_ =
            detail::commit_runtime_bridge_selection(bridge_event_state_);
        }
        if (candidates.capture_exclusion) {
          initial_capture_exclusion_.reset();
        }
        if (!stop_requested_) {
          if (candidates.failed) {
            pending_failed_event_.reset();
          }
          if (candidates.overlay_close) {
            pending_overlay_close_event_ = false;
            overlay_close_published_ = true;
          }
          if (candidates.source_lost) {
            pending_source_lost_event_ = false;
            source_lost_published_ = true;
          }
          recovery_event_state_ =
            detail::commit_runtime_recovery_events(recovery_event_state_);
        }
        else {
          pending_failed_event_.reset();
          pending_overlay_close_event_ = false;
          pending_source_lost_event_ = false;
          recovery_event_state_.pending_started = false;
          recovery_event_state_.pending_completed = false;
        }
      }
      return result;
    } catch (...) {
      // Discard a partial event batch and retain all owners after an unexpected exception.
      result = {};
      const auto exception = detail::observe_runtime_exception(
        drain_state_,
        stop_requested_ ? detail::RuntimeExceptionPhase::Stopping
                        : detail::RuntimeExceptionPhase::Running,
        now);
      drain_state_ = exception.drain.state;
      result.next_wake = exception.drain.next_deadline;
      if (exception.emit_failure) {
        result.events[0] =
          app::RuntimeEvent{app::Failed{.error = runtime_state_error()}};
      }
      else if (exception.drain.status_change) {
        result.events[0] = app::RuntimeEvent{app::RuntimeDrainStatusChanged{
          .status = *exception.drain.status_change}};
      }
      return result;
    }
  }

}    // namespace airelinna::platform::windows
