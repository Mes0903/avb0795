/// @file
/// @brief Bounded scalar observations for one explicitly requested performance run.
/// Collection stays allocation-free after construction; serialization is explicit.
#ifndef AVB0795_DIAGNOSTICS_PERFORMANCE_REPORT_HPP
#define AVB0795_DIAGNOSTICS_PERFORMANCE_REPORT_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string_view>

namespace avb::diagnostics {
  enum class PerformanceScope { Controller,
                                Replica };
  enum class PerformanceOwner { Controller,
                                Replica };
  enum class PerformanceSample { UiBuild,
                                 IntakeToSubmit,
                                 PresentRetirementInterval };
  enum class PerformanceCounter {
    UiBuilds,
    HiddenSyncs,
    GraphicsOnlyWakes,
    Admitted,
    Submitted,
    PresentRetired,
    Dropped,
    RetainedRedraws,
    D3CopyBytes,
    CpuCopyBytes,
    CpuClearBytes,
    VulkanUploadBytes,
    CommandBuffersCreated,
    CommandBuffersDestroyed,
    FencesCreated,
    FencesDestroyed,
    SemaphoresCreated,
    SemaphoresDestroyed,
  };
  /// @brief Process-local identity locked at the scope's qualifying start event.
  struct RuntimeTag {
    std::uint64_t instance{};
    std::uint64_t generation{};
    friend bool operator==(const RuntimeTag &, const RuntimeTag &) = default;
  };
  /// @brief Scalar snapshot borrowed only for one observation call.
  /// Text is copied into bounded storage (255 bytes per field); truncation makes
  /// the run incomplete. Zero fps_cap denotes an unlimited admission rate.
  struct PerformanceMetadata {
    std::string_view gpu;
    std::string_view driver;
    std::string_view os;
    std::string_view bridge;
    std::string_view bridge_reason;
    std::string_view present_mode;
    std::string_view ui_policy;
    std::string_view requested_bridge;
    std::uint32_t content_width{};
    std::uint32_t content_height{};
    std::uint32_t allocation_width{};
    std::uint32_t allocation_height{};
    std::uint32_t framebuffer_width{};
    std::uint32_t framebuffer_height{};
    std::uint32_t fps_cap{};
  };
  /// @brief Current occupancy, reduced to maxima inside the measurement window.
  /// Zero transient_bytes means unreported and serializes as null.
  struct PerformanceHighWater {
    std::uint64_t command_buffers{};
    std::uint64_t fences{};
    std::uint64_t semaphores{};
    std::uint64_t transient_bytes{};
    std::uint64_t slots{};
    std::uint64_t presents{};
  };
  enum class PerformanceDrainState { NotStarted,
                                     Complete,
                                     Stalled,
                                     Abandoned };
  enum class PerformanceWriteError {
    AlreadyWritten,
    TargetExists,
    OpenFailed,
    WriteFailed,
    FlushFailed,
    CloseFailed,
    SerializationFailed,
  };
  /// @brief One process-lifetime, main-thread collector with three fixed sample arrays.
  /// @pre Calls are serialized by the caller and use one monotonic clock.
  /// Construction allocates 3 x 65,536 nanosecond samples. Observations neither
  /// allocate nor perform I/O. The chosen start event begins 10 s of warmup, then
  /// a fixed 60 s half-open measurement interval; later events cannot reset it.
  /// Identity locks at scope start; owner metadata locks at measurement start.
  /// Changes and capacity overflow retain observations but invalidate the run.
  class PerformanceReport final {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::nanoseconds;
    explicit PerformanceReport(PerformanceScope scope);
    ~PerformanceReport();
    PerformanceReport(const PerformanceReport &) = delete;
    PerformanceReport &operator=(const PerformanceReport &) = delete;
    PerformanceReport(PerformanceReport &&) = delete;
    PerformanceReport &operator=(PerformanceReport &&) = delete;
    /// Add a delta only within the measurement interval; owners remain separate.
    void observe_counter(PerformanceOwner owner, PerformanceCounter counter, std::uint64_t value, TimePoint now) noexcept;
    /// Record a nonnegative duration. Discard the first in-window presentation
    /// interval because its predecessor can belong to warmup rather than the run.
    void observe_sample(PerformanceSample kind, Duration duration, TimePoint now) noexcept;
    void observe_runtime(RuntimeTag tag, TimePoint now) noexcept;
    void observe_first_present(TimePoint now) noexcept;
    void observe_first_ui(TimePoint now) noexcept;
    void observe_metadata(PerformanceOwner owner, const PerformanceMetadata &metadata, TimePoint now) noexcept;
    void observe_high_water(PerformanceOwner owner, PerformanceHighWater values, TimePoint now) noexcept;
    /// Progress idle periods; reaching the fixed end freezes observations.
    void advance(TimePoint now) noexcept;
    /// Record shutdown admission before cleanup can cross the measurement end.
    /// Controller stop ends either scope; replica stop ends only replica scope.
    /// An early relevant stop freezes observations and keeps the run incomplete.
    void observe_stop(PerformanceOwner owner, TimePoint now) noexcept;
    /// Freeze after cleanup, recording early shutdown or unsafe drain.
    /// Repeated calls do not rewrite the first finish result.
    void finish(TimePoint now,
                PerformanceDrainState drain = PerformanceDrainState::Complete) noexcept;
    /// @brief Serialize once using exclusive create; existing files stay untouched.
    /// The filename stem supplies run_id. Failures identify the failed stage;
    /// a partial newly created file can remain. Even a failed attempt consumes
    /// the one-shot write. Only this operation allocates
    /// or performs I/O after construction.
    [[nodiscard]] std::expected<void, PerformanceWriteError>
    write_new(const std::filesystem::path &path);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}    // namespace avb::diagnostics

#endif
