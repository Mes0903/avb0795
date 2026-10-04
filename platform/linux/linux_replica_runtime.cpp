/// @file
/// @brief Ordered Linux capture delivery, graphics repair and retained native drain.
#include "platform/linux/linux_replica_runtime.hpp"
#include "platform/linux/linux_cpu_frame_bridge.hpp"
#include "avb/diagnostics/performance_report.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <utility>
namespace avb::linux_platform {
  namespace {
    using Time = std::chrono::steady_clock::time_point;
    using namespace std::chrono_literals;
    /// @brief Merge owner deadlines; an idle owner contributes no periodic work.
    void earliest(std::optional<Time> &destination, std::optional<Time> candidate) noexcept
    {
      if (candidate && (!destination || *candidate < *destination)) destination = candidate;
    }
    core::AppError lost(core::AppError error) noexcept
    {
      error.code = core::AppErrorCode::RuntimeStateLost;
      return error;
    }
  }    // namespace
  namespace detail {
    std::optional<core::AppError> capture_terminal_failure(const CpuPoolFacts &facts) noexcept
    {
      if (facts.error) {
        if (facts.error->code == core::AppErrorCode::SourceUnavailable) return {};
        return lost(*facts.error);
      }
      if (facts.lost) return core::AppError{core::AppErrorCode::RuntimeStateLost};
      return {};
    }
    bool RetiredContent::observe(const render::PresentationSnapshot snapshot) noexcept
    {
      if (!snapshot.content.width || !snapshot.content.height || !snapshot.framebuffer.width ||
          !snapshot.framebuffer.height || !snapshot.present_sequence || snapshot.content == extent) return false;
      extent = snapshot.content;
      pending = true;
      return true;
    }
    bool RetiredContent::invalidate() noexcept
    {
      const bool changed = std::exchange(extent, core::Extent{}) != core::Extent{};
      pending |= changed;
      return changed;
    }
    std::optional<core::Extent> RetiredContent::take_change(bool lifecycle_pending) noexcept
    {
      if (lifecycle_pending || !std::exchange(pending, false)) return {};
      return extent;
    }
    render::RuntimeDrainDecision RuntimeOwnershipClock::observe(Time now, bool pending, bool unknown, render::DrainProgress progress) noexcept
    {
      if (!pending && !unknown && progress == render::DrainProgress::ProvenSafeRelease)
        uncertain = false;
      else
        uncertain |= unknown;
      if (!last_progress) last_progress = now;
      auto decision = render::RuntimeDrainPolicy{}.evaluate({pending, uncertain, progress,
                                                             std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_progress)});
      if (decision.restart_no_progress_interval) last_progress = now;
      if (decision.may_complete) last_progress.reset();
      return decision;
    }
    core::Extent replica_framebuffer(GLFWwindow *window) noexcept
    {
      int width{}, height{};
      glfwGetFramebufferSize(window, &width, &height);
      return {static_cast<std::uint32_t>(std::max(width, 0)), static_cast<std::uint32_t>(std::max(height, 0))};
    }
    bool is_graphics_device_loss(const core::AppError error) noexcept
    {
      return error.native_detail == static_cast<std::int64_t>(VK_ERROR_DEVICE_LOST);
    }
    std::uint64_t prepare_cpu_ring(render::VulkanRenderer &renderer, core::Extent allocation, std::uint64_t revision)
    {
      auto serial = renderer.begin_ring(allocation);
      if (!serial) {
        if (is_graphics_device_loss(serial.error())) renderer.note_device_loss();
        throw serial.error();
      }
      for (std::uint32_t index{}; index < render::VulkanRenderer::source_slot_capacity; ++index)
        if (auto created = renderer.create_cpu_slot({*serial, index}); !created) {
          if (is_graphics_device_loss(created.error())) renderer.note_device_loss();
          throw created.error();
        }
      if (auto published = renderer.publish_ring(*serial, revision); !published) {
        if (is_graphics_device_loss(published.error())) renderer.note_device_loss();
        throw published.error();
      }
      return *serial;
    }
  }    // namespace detail
  void ReplicaWindowDeleter::operator()(GLFWwindow *window) const noexcept { glfwDestroyWindow(window); }
  LinuxReplicaRuntime::LinuxReplicaRuntime(ConstructionToken, ReplicaWindow window, std::unique_ptr<X11OverlayAdapter> adapter, std::unique_ptr<render::VulkanRenderer> renderer, std::unique_ptr<X11Capture> capture, core::ReplicaSettings settings, std::uint64_t revision, std::uint64_t ring_serial, std::function<void()> wake, diagnostics::PerformanceReport *performance, std::uint64_t observation) noexcept
      : window_{std::move(window)}, adapter_{std::move(adapter)}, renderer_{std::move(renderer)}, capture_{std::move(capture)}, settings_{settings}, scheduler_{settings.fps_cap, std::chrono::steady_clock::now()}, wake_{std::move(wake)}, performance_{performance}, observation_instance_{observation}, producer_revision_{revision}, ring_serial_{ring_serial} {}
  LinuxReplicaRuntime::~LinuxReplicaRuntime()
  {
    // Unpublished owners have never submitted. A published owner must already
    // have reported Stopped; unwinding unknown native ownership is never cleanup.
    if (published_ && !stopped_) std::terminate();
  }
  std::expected<void, core::AppError> LinuxReplicaRuntime::activate_capture() noexcept
  {
    if (published_ || !capture_ || !renderer_)
      return std::unexpected(core::AppError{core::AppErrorCode::RuntimeConstructionFailure});
    if (auto started = capture_->start(); !started) return started;
    published_ = true;
    renderer_->set_source_admission(true);
    return {};
  }
  std::expected<void, core::AppError> LinuxReplicaRuntime::apply(const core::ReplicaSettings &candidate) noexcept
  {
    if (stop_requested_ || stopped_ || terminal_failure_ || source_lost_ || !adapter_)
      return std::unexpected(core::AppError{core::AppErrorCode::InvalidCommand});
    if (candidate.region && content_.extent.width && !core::valid_region(*candidate.region, content_.extent))
      return std::unexpected(core::AppError{core::AppErrorCode::InvalidValue});
    auto accepted = adapter_->apply(candidate);
    if (!accepted) {
      if (accepted.error().code == core::AppErrorCode::RuntimeStateLost) fail(accepted.error());
      return accepted;
    }
    // Renderer/scheduler/settings commitment is value-only after native acceptance.
    if (renderer_) renderer_->set_presentation({candidate.region, candidate.selecting_region, {}});
    if (candidate.fps_cap != settings_.fps_cap) scheduler_ = core::FrameScheduler{candidate.fps_cap, std::chrono::steady_clock::now()};
    settings_ = candidate;
    return {};
  }
  void LinuxReplicaRuntime::invalidate_mapping() noexcept
  {
    retry_frame_.reset();
    if (adapter_) adapter_->set_presentation({});
    (void) content_.invalidate();
    if (settings_.selecting_region && adapter_) {
      auto restored = settings_;
      restored.selecting_region = false;
      auto result = adapter_->apply(restored);
      if (!result) {
        failed_ = lost(result.error());
        terminal_failure_ = true;
      }
      else
        settings_ = restored;
    }
  }
  void LinuxReplicaRuntime::complete_selection() noexcept
  {
    settings_.selecting_region = false;
    if (renderer_) renderer_->set_presentation({settings_.region, false, {}});
  }
  void LinuxReplicaRuntime::fail(core::AppError error) noexcept
  {
    if (!terminal_failure_) failed_ = lost(error);
    terminal_failure_ = true;
    device_recovering_ = false;
    reconfiguring_ = false;
    invalidate_mapping();
    if (capture_) capture_->request_stop();
    if (renderer_) {
      renderer_->set_source_admission(false);
      if (renderer_->vulkan_device_lost()) {
        renderer_->begin_recovery_drain(true);
        loss_drain_applied_ = true;
      }
      else
        renderer_->begin_drain();
    }
  }
  void LinuxReplicaRuntime::request_stop() noexcept
  {
    if (stop_requested_) return;
    stop_requested_ = true;
    device_recovering_ = false;
    reconfiguring_ = false;
    if (performance_) performance_->observe_stop(diagnostics::PerformanceOwner::Replica, std::chrono::steady_clock::now());
    invalidate_mapping();
    if (capture_) capture_->request_stop();
    if (renderer_) {
      renderer_->set_source_admission(false);
      if (renderer_->vulkan_device_lost()) {
        renderer_->begin_recovery_drain(true);
        loss_drain_applied_ = true;
      }
      else
        renderer_->begin_drain();
    }
  }
  void LinuxReplicaRuntime::begin_device_recovery(Time now) noexcept
  {
    if (!renderer_ || loss_drain_applied_) return;
    invalidate_mapping();
    renderer_->set_source_admission(false);
    // Confirmed Vulkan loss does not discharge an outstanding producer reservation.
    // The synchronous CPU bridge returns/cancels its keyed reservation before here.
    renderer_->begin_recovery_drain(true);
    loss_drain_applied_ = true;
    if (stop_requested_ || source_lost_ || terminal_failure_) return;
    device_recovering_ = true;
    reconfiguring_ = false;
    if (!recovery_started_) recovery_started_ = now;
    recovery_started_event_ = true;
  }
  void LinuxReplicaRuntime::progress_reconfiguration(Time now, const detail::CpuPoolFacts &facts, render::DrainProgress &progress)
  {
    if (!renderer_ || !capture_ || stop_requested_ || source_lost_ || terminal_failure_ || device_recovering_) return;
    if (facts.revision != producer_revision_ && !reconfiguring_) {
      retry_frame_.reset();
      renderer_->set_source_admission(false);
      renderer_->discard_ready_inputs();
      invalidate_mapping();
      reconfiguring_ = true;
    }
    if (!reconfiguring_ || !facts.format || facts.storage_ready_revision != facts.revision || !facts.revision) return;
    // storage_ready proves old producer leases/writers ended. Successful detach,
    // not the idle hint, grants release of image registrations and ring resources.
    if (ring_serial_) {
      if (!renderer_->render_allocation_idle()) return;
      auto detached = renderer_->detach_ring(ring_serial_);
      if (!detached) {
        // Descriptor reset is a known Vulkan-origin result. Latch that loss
        // before recovery; unrelated producer/native integers never reach here.
        if (detail::is_graphics_device_loss(detached.error())) renderer_->note_device_loss();
        if (renderer_->vulkan_device_lost()) {
          begin_device_recovery(now);
          return;
        }
        fail(detached.error());
        ownership_clock_.uncertain = true;
        return;
      }
      renderer_->release_ring_resources();
      ring_serial_ = 0;
      progress = render::DrainProgress::ProvenSafeRelease;
    }
    ring_serial_ = detail::prepare_cpu_ring(*renderer_, facts.format->allocation, facts.revision);
    // A negotiation racing preparation must never reopen this stale installation.
    auto acknowledged = capture_->acknowledge_allocation_revision(facts.revision);
    if (!acknowledged) {
      const auto current = capture_->progress(std::chrono::steady_clock::now()).frame;
      if (!current.lost && !current.stopping && !current.error && current.revision != facts.revision) {
        renderer_->discard_ready_inputs();
        return;
      }
      fail(acknowledged.error());
      return;
    }
    producer_revision_ = facts.revision;
    reconfiguring_ = false;
    renderer_->set_source_admission(true);
  }
  void LinuxReplicaRuntime::progress_device_recovery(Time now, const detail::CpuPoolFacts &facts, render::VulkanTickResult &graphics, render::DrainProgress &progress, std::optional<Time> &deadline)
  {
    if (!device_recovering_ || stop_requested_ || source_lost_ || terminal_failure_) return;
    if (renderer_) {
      if (!graphics.drain_complete) return;
      renderer_->detach_for_shutdown();
      renderer_.reset();
      ring_serial_ = 0;
      prior_slots_ = prior_presents_ = 0;
      progress = render::DrainProgress::ProvenSafeRelease;
    }
    if (!facts.format || !facts.revision || facts.storage_ready_revision != facts.revision) {
      earliest(deadline, now + 2ms);
      return;
    }
    const auto decision = render::GraphicsRetryPolicy{}.evaluate({render::GraphicsFailureCategory::Device,
                                                                  recovery_episode_.attempt, graphics_epoch_, std::chrono::duration_cast<std::chrono::milliseconds>(now - *recovery_started_)});
    if (decision.action == render::GraphicsRetryAction::Terminal) {
      fail(core::AppError{core::AppErrorCode::RuntimeStateLost});
      return;
    }
    if (*decision.retry_after > 0ms) {
      earliest(deadline, now + *decision.retry_after);
      return;
    }
    recovery_episode_ = render::advance_graphics_recovery_episode(recovery_episode_, render::GraphicsRecoveryObservation::ConstructionAttempted);
    try {
      // All replacement owners are local and never submitted until exact admission.
      auto candidate = std::make_unique<render::VulkanRenderer>(window_.get(), facts.revision,
                                                                detail::replica_framebuffer(window_.get()), render::VulkanStartupRequest{}, performance_, observation_instance_);
      candidate->set_source_admission(false);
      const auto serial = detail::prepare_cpu_ring(*candidate, facts.format->allocation, facts.revision);
      candidate->set_presentation({settings_.region, settings_.selecting_region, {}});
      const auto current = capture_->progress(now).frame;
      if (current.lost || current.stopping || current.error) {
        earliest(deadline, now + 1ms);
        return;
      }
      if (current.revision != facts.revision || current.storage_ready_revision != facts.revision) {
        earliest(deadline, now + 1ms);
        return;
      }
      // Graphics-only replacement preserves capture binding. A producer revision
      // transition uses the separate exact storage-ready acknowledgment gate.
      if (facts.revision != producer_revision_) {
        if (auto acknowledged = capture_->acknowledge_allocation_revision(facts.revision); !acknowledged) {
          earliest(deadline, now + 1ms);
          return;
        }
      }
      renderer_ = std::move(candidate);
      renderer_presented_baseline_ = 0;    // A new renderer restarts its cumulative retirement count.
      ring_serial_ = serial;
      producer_revision_ = facts.revision;
      graphics_epoch_ = *decision.candidate_generation;
      renderer_->set_source_admission(true);
      loss_drain_applied_ = false;
      device_recovering_ = false;
      recovery_episode_ = render::advance_graphics_recovery_episode(recovery_episode_, render::GraphicsRecoveryObservation::ReplacementConstructed);
      recovered_event_ = true;
      graphics = {};    // Old renderer retirement cannot certify this replacement.
      earliest(deadline, now + 1ms);
    } catch (...) {
      // Constructor/ring failure unwinds only never-submitted candidate graphics;
      // the healthy bounded producer remains retained for the next budgeted attempt.
      earliest(deadline, now + 1ms);
    }
  }
  app::RuntimeTickResult LinuxReplicaRuntime::tick(Time now) noexcept
  {
    app::RuntimeTickResult result;
    if (stopped_) return result;
    std::size_t event_count{};
    auto emit = [&](app::RuntimeEvent event) noexcept { if (event_count<result.events.size()) result.events[event_count++]=event; };
    render::DrainProgress progress = render::DrainProgress::None;
    render::VulkanTickResult graphics;
    X11CaptureProgress producer;
    X11OverlayEvents native;
    std::optional<Time> fps_deadline;
    bool scheduled_work{};
    try {
      // Presentation retirement and drawable invalidation MUST precede fresh CPU delivery.
      if (renderer_) graphics = renderer_->begin_tick(detail::replica_framebuffer(window_.get()), now);
      if (adapter_) native = adapter_->take_events();
      if (native.region_finished) complete_selection();
      if (capture_) {
        producer = capture_->progress(now);
        published_source_sequence_ = producer.frame.sequence;
        observed_producer_revision_ = producer.frame.revision;
        earliest(result.next_wake, producer.next_wake);
      }
      if (native.failure) fail(*native.failure);
      // Producer-local failures have no Vulkan provenance. Classify terminal
      // causes before generic disconnection so deadline cannot emit SourceLost.
      if (!stop_requested_)
        if (auto failure = detail::capture_terminal_failure(producer.frame)) fail(*failure);
      if (producer.frame.lost || producer.frame.error) {
        if (!source_lost_ && !stop_requested_ && !terminal_failure_) {
          source_lost_ = true;
          device_recovering_ = false;
          reconfiguring_ = false;
          invalidate_mapping();
          if (capture_) capture_->request_stop();
          if (renderer_) renderer_->begin_drain();
        }
      }
      if (graphics.vulkan_device_lost) begin_device_recovery(now);
      if (graphics.terminal_error && !graphics.vulkan_device_lost) {
        fail(*graphics.terminal_error);
        ownership_clock_.uncertain = true;
      }
      if (graphics.presentation_suspended) invalidate_mapping();
      // Revision facts close admission now; new ring creation waits until AFTER
      // this pass's submission proof, so producer delivery cannot outrun retirement.
      if (renderer_ && !device_recovering_ && producer.frame.revision != producer_revision_ && !reconfiguring_ &&
          !stop_requested_ && !source_lost_ && !terminal_failure_) {
        reconfiguring_ = true;
        retry_frame_.reset();
        renderer_->set_source_admission(false);
        renderer_->discard_ready_inputs();
        invalidate_mapping();
      }
      const bool live = !stop_requested_ && !source_lost_ && !terminal_failure_ && !device_recovering_ && !reconfiguring_;
      if (live && renderer_ && capture_) {
        if (auto newest = capture_->take_latest()) {
          retry_frame_.reset();
          retry_frame_ = std::move(newest);
        }
        if (retry_frame_ && retry_frame_->allocation_revision() != producer_revision_) retry_frame_.reset();
        renderer_->set_presentation({settings_.region, settings_.selecting_region, adapter_->selection_preview()});
        const auto schedule = scheduler_.poll(now, retry_frame_.has_value(), renderer_->has_redraw_work());
        fps_deadline = schedule.next_wake;
        scheduled_work = schedule.render;
        earliest(result.next_wake, schedule.next_wake);
        if (schedule.render) {
          if (retry_frame_) {
            auto delivered = LinuxCpuFrameBridge::deliver(*renderer_, *retry_frame_);
            if (!delivered) {
              retry_frame_.reset();
              // Only the renderer's typed Vulkan loss latch authorizes device loss.
              // Numeric producer/native error codes never enter a VkResult classifier.
              if (renderer_->vulkan_device_lost())
                begin_device_recovery(now);
              else
                fail(delivered.error());
            }
            else if (*delivered) {
              delivered_stamp_ = {retry_frame_->sequence(), retry_frame_->intake_observed_at()};
              retry_frame_.reset();
            }
          }
          else
            renderer_->request_redraw();
        }
      }
      if (renderer_) {
        renderer_->retire_submissions(graphics, now);
        if (graphics.slots_in_flight < prior_slots_ || graphics.presentations_in_flight < prior_presents_)
          progress = render::DrainProgress::TrackedRetirement;
        if (renderer_->vulkan_device_lost()) begin_device_recovery(now);
        if (graphics.terminal_error && !renderer_->vulkan_device_lost()) {
          fail(*graphics.terminal_error);
          ownership_clock_.uncertain = true;
        }
        // FPS controls work admission, not this aggregate allow-present flag.
        // WSI progress includes release-retained destruction during drain, not
        // just repair construction. Renderer draining/loss state governs that
        // cleanup; aggregate device replacement remains gated separately.
        renderer_->finish_tick(graphics, now, !stop_requested_ && !source_lost_ && !terminal_failure_ && !device_recovering_ && !reconfiguring_);
        // finish_tick reports this renderer's cumulative successful fence retirements.
        // Repeated polls and ring/content changes add no new retirement; only an
        // installed replacement resets the baseline, preserving the runtime total.
        if (graphics.presented_frames > renderer_presented_baseline_) {
          successful_present_retirements_ += graphics.presented_frames - renderer_presented_baseline_;
          renderer_presented_baseline_ = graphics.presented_frames;
        }
        earliest(result.next_wake, graphics.next_deadline);
        if (graphics.vulkan_device_lost) begin_device_recovery(now);
        if (graphics.terminal_error && !graphics.vulkan_device_lost) {
          fail(*graphics.terminal_error);
          ownership_clock_.uncertain = true;
        }
        if (graphics.surface_recovery_started) {
          invalidate_mapping();
          recovery_started_event_ = true;
        }
        if (graphics.surface_recovery_completed) recovered_event_ = true;
        if (graphics.presentation_suspended) invalidate_mapping();
        if (!stop_requested_ && !source_lost_ && !terminal_failure_ && !device_recovering_ && !reconfiguring_ && graphics.presentation) {
          const bool content_changed = content_.extent.width && content_.extent != graphics.presentation->content;
          if (content_changed && settings_.selecting_region) {
            auto restored = settings_;
            restored.selecting_region = false;
            // ContentChanged certifies cancellation. The application need not issue
            // another apply when the crop intersection leaves its settings equal.
            if (auto accepted = apply(restored); !accepted)
              fail(lost(accepted.error()));
            else {
              native.region_finished = true;
              native.region.reset();
            }
          }
          if (!terminal_failure_) {
            adapter_->set_presentation(*graphics.presentation);
            (void) content_.observe(*graphics.presentation);
          }
        }
        if (recovery_episode_.awaiting_successful_present && !device_recovering_ && !graphics.vulkan_device_lost && graphics.presented_frames) {
          recovery_episode_ = render::advance_graphics_recovery_episode(recovery_episode_, render::GraphicsRecoveryObservation::SuccessfulPresentRetired);
          recovery_started_.reset();
        }
        prior_slots_ = graphics.slots_in_flight;
        prior_presents_ = graphics.presentations_in_flight;
        renderer_->observe_metadata({"Linux", "cpu_copy", "none", "cpu_copy", content_.extent, settings_.fps_cap}, now);
        // A retirement can make retained work reusable after the earlier poll.
        // Preserve a capped admission deadline instead of spinning every millisecond.
        if (live && !fps_deadline && (retry_frame_ || (scheduled_work && renderer_->has_redraw_work())))
          earliest(result.next_wake, now + 1ms);
      }
      progress_reconfiguration(now, producer.frame, progress);
      progress_device_recovery(now, producer.frame, graphics, progress, result.next_wake);
      // Mapping publication can cancel selection or fail native restoration. Consume
      // these facts in this same tick before exposing content or region completion.
      if (adapter_) {
        const auto mapping = adapter_->take_events();
        native.close_requested |= mapping.close_requested;
        if (mapping.region_finished) {
          native.region_finished = true;
          native.region = mapping.region;
        }
        if (mapping.failure) fail(*mapping.failure);
        if (native.region_finished && !terminal_failure_) {
          complete_selection();
          earliest(result.next_wake, now + 1ms);
        }
      }
      if (capture_ && producer.quiescent && (stop_requested_ || source_lost_ || terminal_failure_)) {
        retry_frame_.reset();
        capture_.reset();
        progress = render::DrainProgress::ProvenSafeRelease;
      }
      if (stop_requested_ && renderer_ && graphics.drain_complete) {
        renderer_->detach_for_shutdown();
        renderer_.reset();
        ring_serial_ = 0;
        progress = render::DrainProgress::ProvenSafeRelease;
      }
      const bool ownership_pending = (capture_ && (stop_requested_ || source_lost_ || terminal_failure_)) ||
                                     (renderer_ && (stop_requested_ || (source_lost_ && !graphics.drain_complete) || (terminal_failure_ && !graphics.drain_complete))) ||
                                     device_recovering_ || reconfiguring_;
      // Evaluate even when this pass completed the last transition. Actual
      // detach/loss retirement and exact revision acknowledgment clear its flags;
      // a constructor or retry alone does not withdraw pending ownership.
      {
        const bool empty = !capture_ && !renderer_ && stop_requested_;
        const auto decision = ownership_clock_.observe(now, ownership_pending, false,
                                                       empty ? render::DrainProgress::ProvenSafeRelease : progress);
        if (decision.status != drain_status_) {
          drain_status_ = decision.status;
          emit(app::RuntimeDrainStatusChanged{drain_status_});
        }
        if (decision.poll_after) earliest(result.next_wake, now + *decision.poll_after);
        if (stop_requested_ && empty && decision.may_complete) {
          // Adapter callback reachability ends before GLFW destruction. No producer
          // or renderer wake/native borrow remains when host receives Stopped.
          adapter_.reset();
          window_.reset();
          wake_ = {};
          stopped_ = true;
          result.events = {};
          result.events[0] = app::Stopped{};
          result.next_wake.reset();
          return result;
        }
      }
    } catch (const core::AppError &error) {
      // The ring helper latches only its known graphics-origin error. Keep the
      // healthy producer for loss drain/rebuild, including partially built slots.
      if (renderer_ && renderer_->vulkan_device_lost())
        begin_device_recovery(now);
      else
        fail(error);
      earliest(result.next_wake, now + 2ms);
    } catch (...) {
      fail(core::AppError{core::AppErrorCode::RuntimeStateLost});
      earliest(result.next_wake, now + 2ms);
    }
    if (initial_status_) {
      initial_status_ = false;
      emit(app::BridgeModeChanged{core::BridgeMode::CpuCopy, core::BridgeReason::None});
      emit(app::OverlayCaptureExclusionChanged{core::CaptureExclusionStatus::NotAttempted});
    }
    if (!stop_requested_) {
      const bool lifecycle_pending = failed_.has_value() || native.close_requested || source_lost_ || recovery_started_event_ || recovered_event_;
      if (failed_) emit(app::Failed{*std::exchange(failed_, {})});
      if (native.close_requested && !close_reported_) {
        close_reported_ = true;
        emit(app::OverlayCloseRequested{});
      }
      if (source_lost_ && !source_lost_reported_) {
        source_lost_reported_ = true;
        emit(app::SourceLost{});
      }
      if (std::exchange(recovery_started_event_, false)) emit(app::GraphicsRecoveryStarted{});
      if (std::exchange(recovered_event_, false)) emit(app::GraphicsRecovered{});
      // the application ignores geometry in a lifecycle tick. Keep the latest content
      // edge and ask for another observation once that transition is published.
      if (auto changed = content_.take_change(lifecycle_pending)) emit(app::CaptureContentChanged{*changed});
      if (content_.pending && !source_lost_ && !terminal_failure_) earliest(result.next_wake, now + 1ms);
      if (native.region_finished && !terminal_failure_) {
        emit(app::RegionSelectionCompleted{native.region});
      }
    }
    return result;
  }
  LinuxSourceObservation LinuxReplicaRuntime::source_observation() const noexcept
  {
    return {observation_instance_, observed_producer_revision_, graphics_epoch_,
            published_source_sequence_, successful_present_retirements_, delivered_stamp_, stopped_};
  }
}    // namespace avb::linux_platform
