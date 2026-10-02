/// @file
/// @brief Native admission remains owned until first-frame or cleanup acknowledgments.
#include "platform/linux/linux_source_acquisition.hpp"
#include <algorithm>
#include <limits>
#include <utility>
namespace avb::linux_platform {
  namespace {
    constexpr core::AppError invalid{core::AppErrorCode::InvalidCommand};
    constexpr core::AppError construction{core::AppErrorCode::RuntimeConstructionFailure};
    constexpr core::AppError unavailable{core::AppErrorCode::SourceUnavailable};
  }    // namespace
  LinuxSourceAcquisition::LinuxSourceAcquisition(LinuxSourceRegistry &registry, std::string parent, std::function<void()> wake)
      : registry_{registry}, parent_{std::move(parent)}, wake_{std::move(wake)}, portal_{std::make_unique<PortalScreencast>(parent_, wake_)} {}
  LinuxSourceAcquisition::~LinuxSourceAcquisition() = default;
  std::expected<void, core::AppError> LinuxSourceAcquisition::begin(const app::SourceAcquisitionRequest &request)
  {
    if (shutdown_ || request_) return std::unexpected(invalid);
    std::optional<PortalResumeTarget> resume;
    LinuxSourceRegistry::Record *record{};
    if (request.resume_source) {
      auto target = registry_.resume_target(*request.resume_source);
      if (!target) return std::unexpected(target.error());
      resume = std::move(*target);
      std::lock_guard lock{registry_.mutex_};
      record = registry_.find(*request.resume_source);
      if (record->capture) return std::unexpected(invalid);
    }
    // Initial GetAll failure leaves a retryable worker: keep that same service.
    // A terminal transport/owner loss retires it completely. Only a later user
    // operation after that acknowledgment may create another inactive service.
    if (portal_started_ && portal_quiescent_) {
      if (request.resume_source) return std::unexpected(unavailable);
      {
        std::lock_guard lock{registry_.mutex_};
        // Portal exit alone does not retire the independent prepared producer's
        // callbacks/wake. A later user operation must wait for both owner domains.
        if (std::any_of(registry_.records_.begin(), registry_.records_.end(),
                        [](const auto &old) { return bool(old.capture); })) return std::unexpected(invalid);
      }
      portal_ = std::make_unique<PortalScreencast>(parent_, wake_);
      portal_started_ = false;
    }
    auto owned_request = request;    // All fallible request storage precedes activation.
    auto accepted = portal_->begin(owned_request, std::move(resume));
    if (!accepted) return accepted;
    request_ = std::move(owned_request);
    preparing_ = record;
    failure_.reset();
    discarded_session_ = {};
    first_frame_deadline_.reset();
    announced_phase_.reset();
    cancelled_ = false;
    operation_retired_ = false;
    cancel_submitted_ = false;
    phase_ = request.resume_source ? app::AcquisitionPhase::PreparingStream : app::AcquisitionPhase::AwaitingPermission;
    if (request.resume_source) first_frame_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    portal_started_ = true;
    portal_quiescent_ = false;
    return {};
  }
  void LinuxSourceAcquisition::cancel(app::AcquisitionId id) noexcept
  {
    if (!request_ || request_->id != id || cancelled_) return;
    cancelled_ = true;
    phase_ = app::AcquisitionPhase::Cancelling;
    cancel_submitted_ = true;
    portal_->cancel(id);
    std::lock_guard lock{registry_.mutex_};
    if (preparing_) {
      preparing_->prepared_serial = {};
      preparing_->revision = 0;
      if (preparing_->capture) {
        preparing_->retiring = true;
        preparing_->capture->request_stop();
      }
      if (preparing_->provisional) preparing_->request_close_once();
    }
  }
  void LinuxSourceAcquisition::request_shutdown() noexcept
  {
    if (shutdown_) return;
    shutdown_ = true;
    if (request_) cancel(request_->id);
    std::lock_guard lock{registry_.mutex_};
    for (auto &record : registry_.records_) {
      record.prepared_serial = {};
      record.revision = 0;
      record.request_close_once();
      if (record.capture) {
        record.retiring = true;
        record.capture->request_stop();
      }
    }
    portal_->request_shutdown();
  }
  app::SourceAcquisitionTickResult LinuxSourceAcquisition::tick(std::chrono::steady_clock::time_point now) noexcept
  {
    app::SourceAcquisitionTickResult result;
    unsigned count{};
    auto event = [&](app::AcquisitionEvent value) noexcept { result.events[count++] = std::move(value); };
    auto deadline = [&](std::optional<std::chrono::steady_clock::time_point> value) noexcept {
      if (value && (!result.next_wake || *value < *result.next_wake)) result.next_wake = value;
    };
    auto portal = portal_->progress(now);
    portal_quiescent_ = portal.quiescent;
    result.made_progress = portal.made_progress;
    deadline(portal.next_wake);
    std::lock_guard lock{registry_.mutex_};
    result.made_progress |= std::exchange(registry_.observed_native_progress_, false);
    if (request_ && portal.id == request_->id) {
      if (portal.cancelled || portal.error) {
        operation_retired_ = true;
        if (portal.error && !cancelled_ && !failure_) failure_ = portal.error;
        // Internal timeout cleanup also uses Portal cancellation. Its native
        // acknowledgment cannot turn the original failure into user cancellation.
        if (portal.cancelled && !failure_) cancelled_ = true;
      }
      if (!cancelled_ && !failure_ && portal.phase == app::AcquisitionPhase::PreparingStream) {
        phase_ = app::AcquisitionPhase::PreparingStream;
        if (!first_frame_deadline_) first_frame_deadline_ = now + std::chrono::seconds{10};
      }
      if (!cancelled_ && !failure_ && first_frame_deadline_ && now >= *first_frame_deadline_) failure_ = construction;
      if (portal.connection) {
        operation_retired_ = true;
        if (cancelled_ || failure_ || shutdown_) {
          if (portal.close_authority) {
            discarded_session_ = portal.connection->session;
            portal.close_authority->request_close();
          }
        }
        else {
          try {
            if (!preparing_) {
              if (registry_.next_source_ == std::numeric_limits<std::uint64_t>::max()) throw construction;
              core::SourceDescriptor source;
              source.id.value = "linux-source-" + std::to_string(++registry_.next_source_);
              source.kind = request_->kind;
              source.display_label = "System selected source";
              source.color_support = core::ColorSupport::Sdr;
              registry_.records_.emplace_back();
              preparing_ = &registry_.records_.back();
              preparing_->source = std::move(source);
              preparing_->provisional = true;
              preparing_->session = portal.connection->session;
              preparing_->node = portal.connection->node_id;
              preparing_->stream_serial = portal.connection->serial;
              preparing_->close = std::move(*portal.close_authority);
            }
            // Registry storage and session cleanup authority exist before the
            // inactive capture is created, and the owner is retained before start.
            auto capture = PipeWireCapture::create(std::move(*portal.connection), wake_);
            if (!capture)
              failure_ = capture.error();
            else {
              preparing_->capture = std::move(*capture);
              preparing_->retiring = false;
              auto started = preparing_->capture->start();
              if (!started) failure_ = started.error();
            }
          } catch (...) {
            failure_ = construction;
            // Record construction can fail before registry storage owns the session.
            // Keep its lifetime fact until native cleanup acknowledges retirement.
            if (!preparing_ && portal.close_authority) discarded_session_ = portal.connection->session;
            if (portal.close_authority) portal.close_authority->request_close();
          }
        }
      }
    }
    else if (portal.connection) {
      // A stale native result never changes selection or activates a producer.
      if (portal.close_authority) portal.close_authority->request_close();
    }
    bool captures_quiescent = true;
    for (auto &record : registry_.records_) {
      if (!record.session.valid() && record.capture) {
        record.retiring = true;
        record.prepared_serial = {};
        record.capture->request_stop();
        if (&record == preparing_ && !cancelled_ && !failure_) failure_ = unavailable;
      }
      if (!record.capture) continue;
      auto progress = record.capture->progress(now);
      result.made_progress |= progress.made_progress;
      deadline(progress.next_wake);
      if (&record == preparing_ && request_ && !cancelled_ && !failure_) {
        if (progress.frame.error)
          failure_ = progress.frame.error;
        else if (progress.frame.lost || progress.frame.stopping)
          failure_ = unavailable;
        else if (first_frame_deadline_ && now >= *first_frame_deadline_)
          failure_ = construction;
        else if (progress.frame.revision && progress.frame.format && progress.frame.sequence &&
                 progress.frame.first_frame_revision == progress.frame.revision &&
                 progress.frame.storage_ready_revision == progress.frame.revision &&
                 progress.frame.content.width && progress.frame.content.height) {
          if (registry_.next_prepared_ == std::numeric_limits<std::uint64_t>::max())
            failure_ = construction;
          else {
            try {
              // Construct the complete portable publication before changing current
              // selection. Allocation failure cannot replace a healthy old record.
              auto descriptor = record.source;
              descriptor.extent = progress.frame.content;
              app::AcquisitionReady ready{request_->id, std::move(descriptor)};
              record.source.extent = progress.frame.content;
              record.prepared_serial = PreparedSourceSerial{++registry_.next_prepared_};
              record.revision = progress.frame.revision;
              if (record.provisional) {
                for (auto &old : registry_.records_)
                  if (&old != &record && old.current) {
                    old.current = false;
                    old.request_close_once();
                    old.prepared_serial = {};
                    if (old.capture) {
                      old.retiring = true;
                      old.capture->request_stop();
                    }
                  }
                record.provisional = false;
                record.current = true;
              }
              event(std::move(ready));
              request_.reset();
              preparing_ = nullptr;
              first_frame_deadline_.reset();
              phase_ = app::AcquisitionPhase::Idle;
            } catch (...) {
              failure_ = construction;
            }
          }
        }
      }
      if (&record != preparing_ && (progress.frame.lost || progress.frame.stopping || progress.frame.error ||
                                    (record.prepared_serial.value_ && progress.frame.revision != record.revision))) {
        // A published preparation cannot keep a dead/stale stream indefinitely
        // when a subsequent factory borrow fails before it obtains a serial.
        record.retiring = true;
        record.prepared_serial = {};
        record.capture->request_stop();
      }
      if (&record == preparing_ && (cancelled_ || failure_)) {
        record.retiring = true;
        record.prepared_serial = {};
        record.capture->request_stop();
        if (record.provisional) record.request_close_once();
      }
      if (record.retiring && progress.quiescent) {
        record.capture.reset();
        record.retiring = false;
        result.made_progress = true;
      }
      else
        captures_quiescent = false;
    }
    if (request_ && failure_ && !operation_retired_ && !cancel_submitted_) {
      // A pending native cleanup gets one intention. Reissuing Cancel on every
      // poll would acknowledge duplicate commands and mask real owner stalling.
      cancel_submitted_ = true;
      portal_->cancel(request_->id);
    }
    if (request_ && (cancelled_ || failure_) && operation_retired_ && (!preparing_ || !preparing_->capture) && discarded_session_.retired()) {
      if (preparing_ && preparing_->provisional) preparing_->request_close_once();
      // A fresh started session belongs to the rejected operation until Close is
      // acknowledged. Retained resume sessions deliberately survive cancellation.
      if (!preparing_ || !preparing_->provisional || preparing_->session.retired()) {
        if (cancelled_)
          event(app::AcquisitionCancelled{request_->id});
        else
          event(app::AcquisitionFailed{request_->id, *failure_});
        request_.reset();
        preparing_ = nullptr;
        first_frame_deadline_.reset();
        phase_ = app::AcquisitionPhase::Idle;
      }
    }
    if (request_ && (!announced_phase_ || *announced_phase_ != phase_)) {
      event(app::AcquisitionPhaseChanged{request_->id, phase_});
      announced_phase_ = phase_;
    }
    // Retired predecessors need no registry-owned native reference. Closed
    // current identities remain observable as terminal resume failures.
    registry_.records_.remove_if([&](const auto &record) {
      return &record != preparing_ && !record.current && !record.capture && record.session.retired();
    });
    result.quiescent = portal_quiescent_ && captures_quiescent && !request_;
    if (!result.quiescent && (request_ || shutdown_ || !captures_quiescent)) deadline(now + std::chrono::milliseconds{10});
    if (request_ && !cancelled_ && !failure_) deadline(first_frame_deadline_);
    return result;
  }
}    // namespace avb::linux_platform
