/// @file
/// @brief Owning source snapshots and synchronized, allocation-free prepared transfer.
#include "platform/linux/linux_source_registry.hpp"
#include <algorithm>
namespace avb::linux_platform {
  namespace {
    constexpr core::AppError unavailable{core::AppErrorCode::SourceUnavailable};
    constexpr core::AppError unprepared{core::AppErrorCode::RuntimeConstructionFailure};
    // Snapshot readiness is deliberately weaker than the final guarded bind. A
    // negotiation can invalidate it immediately after this observation.
    bool ready(const detail::CpuPoolFacts &frame, std::uint64_t revision) noexcept
    {
      return revision && frame.revision == revision && frame.storage_ready_revision == revision &&
             frame.first_frame_revision == revision && frame.format && frame.sequence &&
             frame.content.width && frame.content.height && !frame.stopping && !frame.lost && !frame.error;
    }
  }    // namespace
  LinuxSourceRegistry::Record *LinuxSourceRegistry::find(const core::SourceId &id) noexcept
  {
    auto found = std::find_if(records_.begin(), records_.end(), [&](const auto &record) { return record.source.id == id; });
    return found == records_.end() ? nullptr : &*found;
  }
  const LinuxSourceRegistry::Record *LinuxSourceRegistry::find(const core::SourceId &id) const noexcept
  {
    auto found = std::find_if(records_.begin(), records_.end(), [&](const auto &record) { return record.source.id == id; });
    return found == records_.end() ? nullptr : &*found;
  }
  std::vector<core::SourceDescriptor> LinuxSourceRegistry::snapshot() const
  {
    std::lock_guard lock{mutex_};
    std::vector<core::SourceDescriptor> result;
    for (const auto &record : records_)
      if (record.current && record.session.valid()) result.push_back(record.source);
    return result;
  }
  std::expected<PortalResumeTarget, core::AppError> LinuxSourceRegistry::resume_target(const core::SourceId &id) const noexcept
  {
    std::lock_guard lock{mutex_};
    const auto *record = find(id);
    if (!record || !record->current || !record->session.valid()) return std::unexpected(unavailable);
    return PortalResumeTarget{record->session, record->node, record->stream_serial};
  }
  std::expected<PreparedSourceBorrow, core::AppError> LinuxSourceRegistry::borrow_prepared(const core::SourceId &id) noexcept
  {
    std::lock_guard lock{mutex_};
    auto *record = find(id);
    if (!record || !record->session.valid()) return std::unexpected(unavailable);
    if (!record->capture || record->retiring || !record->prepared_serial.value_) return std::unexpected(unprepared);
    const auto progress = record->capture->progress(std::chrono::steady_clock::now());
    observed_native_progress_ |= progress.made_progress;
    const auto &frame = progress.frame;
    if (!ready(frame, record->revision)) return std::unexpected(unprepared);
    return PreparedSourceBorrow{record->prepared_serial, record->revision, frame.content,
                                frame.format->allocation, frame.format->pixels, record->capture.get()};
  }
  std::expected<PreparedLinuxSource, core::AppError> LinuxSourceRegistry::commit_prepared(
    const core::SourceId &id, PreparedSourceSerial serial, std::uint64_t revision) noexcept
  {
    std::lock_guard lock{mutex_};
    auto *record = find(id);
    if (!record || !record->session.valid()) return std::unexpected(unavailable);
    if (!serial.value_ || !record->capture || record->retiring ||
        record->prepared_serial != serial || record->revision != revision) return std::unexpected(unprepared);
    // The pool guard performs the joint *current* negotiation/Stop/first-frame
    // check. No native-loop wait, allocation or fallible work follows its success.
    auto binding = record->capture->bind_current_first_frame(revision);
    if (!binding) return std::unexpected(binding.error());
    PreparedLinuxSource result{std::move(record->capture)};
    record->prepared_serial = {};
    record->revision = 0;
    observed_native_progress_ = true;    // Actual transfer retires this service obligation.
    return result;
    // binding unlocks on this main thread before the result reaches its consumer.
  }
  void LinuxSourceRegistry::reject_prepared(const core::SourceId &id, PreparedSourceSerial serial) noexcept
  {
    std::lock_guard lock{mutex_};
    auto *record = find(id);
    if (!record || !serial.value_ || record->prepared_serial != serial || !record->capture) return;
    record->prepared_serial = {};
    record->revision = 0;
    record->retiring = true;
    record->capture->request_stop();
  }
  std::expected<std::vector<core::SourceDescriptor>, core::AppError> LinuxSourceCatalog::enumerate()
  {
    try {
      return registry_.snapshot();
    } catch (...) {
      return std::unexpected(core::AppError{core::AppErrorCode::SourceCatalogFailure});
    }
  }
}    // namespace avb::linux_platform
