/// @file
/// @brief Main-thread registry of started sessions and uncommitted native capture owners.
#ifndef AVB0795_LINUX_SOURCE_REGISTRY_HPP
#define AVB0795_LINUX_SOURCE_REGISTRY_HPP
#include "avb/app/ports.hpp"
#include "platform/linux/pipewire_capture.hpp"
#include <list>
#include <mutex>
namespace avb::linux_platform {
  class LinuxSourceAcquisition;
  /// @brief Opaque registry-issued identity for one legal preparation, never a native handle.
  /// Default construction is invalid. Retaining a serial grants no lifetime or transfer authority.
  class PreparedSourceSerial final {
  public:
    PreparedSourceSerial() = default;
    friend bool operator==(PreparedSourceSerial, PreparedSourceSerial) = default;

  private:
    explicit PreparedSourceSerial(std::uint64_t value) noexcept : value_{value} {}
    std::uint64_t value_{};
    friend class LinuxSourceRegistry;
    friend class LinuxSourceAcquisition;
  };
  /// @brief Short main-thread borrow for fallible graphics preparation before final commit.
  /// Facts describe the legal frame observed at borrowing, not future negotiation. The
  /// capture remains registry-owned: do not stop, destroy, move or retain it beyond the
  /// immediate factory transaction. Recheck serial/revision with commit before attachment.
  struct PreparedSourceBorrow {
    PreparedSourceSerial serial;
    std::uint64_t allocation_revision{};
    core::Extent content_extent{}, allocation_extent{};
    detail::CpuPixelFormat format{};
    PipeWireCapture *capture{};
  };
  /// @brief Move-only typed capture ownership after the final synchronized first-frame bind.
  /// A successful commit has already bound this capture. Only noexcept attachment may
  /// follow; its consumer must asynchronously Stop/progress to quiescence before destruction.
  /// The registry retains session Close authority independently of this consumer owner.
  struct PreparedLinuxSource {
    std::unique_ptr<PipeWireCapture> capture;
  };
  /// @brief Own started-session Close authority and every not-yet-transferred capture.
  /// All entry points run on the composition main thread. The mutex orders final commit
  /// before the producer's concrete pool guard; callbacks never acquire this mutex.
  /// Rejection schedules Stop while retaining the owner. Acquisition pumps retirement;
  /// neither failed factory construction nor registry destruction may unwind live workers.
  /// @pre Shutdown acquisition and observe true service quiescence before destruction.
  class LinuxSourceRegistry final {
  public:
    [[nodiscard]] std::vector<core::SourceDescriptor> snapshot() const;
    [[nodiscard]] std::expected<PortalResumeTarget, core::AppError> resume_target(const core::SourceId &) const noexcept;
    [[nodiscard]] std::expected<PreparedSourceBorrow, core::AppError> borrow_prepared(const core::SourceId &) noexcept;
    /// @brief Jointly validate serial, current revision and legal first frame, then bind/move.
    /// Failure preserves ownership. Success allocates nothing; the concrete binding guard
    /// ends on this thread before return, so the consumer can safely call capture methods.
    [[nodiscard]] std::expected<PreparedLinuxSource, core::AppError> commit_prepared(const core::SourceId &, PreparedSourceSerial, std::uint64_t) noexcept;
    /// @brief Revoke only the matching preparation without consuming or destructing its owner.
    /// Stale serials cannot reject a newer preparation. Native retirement is polled later.
    void reject_prepared(const core::SourceId &, PreparedSourceSerial) noexcept;

  private:
    /// @brief One started session can outlive successive consumer streams with the same ID.
    /// Current records remain catalog-visible after capture transfer/rejection. Provisional
    /// fresh records cannot replace that selection until a legal frame authorizes Ready.
    struct Record {
      core::SourceDescriptor source;
      PortalSessionLease session;
      PortalSessionCloseAuthority close;
      std::uint32_t node{};
      std::optional<std::uint64_t> stream_serial;
      std::unique_ptr<PipeWireCapture> capture;
      PreparedSourceSerial prepared_serial;
      std::uint64_t revision{};
      bool current{}, retiring{}, provisional{}, close_requested{};
      /// @brief Submit this session's cleanup intention once while retaining its authority.
      /// Duplicate command acknowledgments are not native retirement progress. Calls
      /// run on the main thread under registry synchronization; completion stays native.
      void request_close_once() noexcept
      {
        if (close_requested) return;
        close_requested = true;
        close.request_close();
      }
    };
    Record *find(const core::SourceId &) noexcept;
    const Record *find(const core::SourceId &) const noexcept;
    mutable std::mutex mutex_;
    std::list<Record> records_;
    std::uint64_t next_source_{}, next_prepared_{};
    bool observed_native_progress_{};
    friend class LinuxSourceAcquisition;
  };
  /// @brief Enumerate only owning registry observations; refreshing never opens a chooser.
  /// Snapshot strings outlive this call. It creates neither sessions nor capture owners.
  class LinuxSourceCatalog final : public app::ISourceCatalog {
  public:
    explicit LinuxSourceCatalog(LinuxSourceRegistry &registry) noexcept : registry_{registry} {}
    std::expected<std::vector<core::SourceDescriptor>, core::AppError> enumerate() override;

  private:
    LinuxSourceRegistry &registry_;
  };
}    // namespace avb::linux_platform
#endif
