/// @file
/// @brief Concrete Linux X11 producer and bounded main-thread observations.
#ifndef AVB0795_LINUX_X11_CAPTURE_HPP
#define AVB0795_LINUX_X11_CAPTURE_HPP
#include "platform/linux/detail/cpu_frame_pool.hpp"
#include "platform/linux/x11_source_catalog.hpp"
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
namespace avb::linux_platform {
  namespace detail {
    /// @brief Keep source lifetime, native resource and byte allocation identities separate.
    /// Recreating a pixmap can invalidate an image without changing pool revision.
    /// The worker checks this stamp after draining events and before reserving bytes;
    /// the pool remains authoritative for bound-ring admission and commit races.
    struct X11RequestStamp { std::uint64_t observation_generation{}, resource_epoch{}, allocation_revision{}; };
    [[nodiscard]] bool x11_request_current(const X11RequestStamp &, std::uint64_t generation,
                                           std::uint64_t resource_epoch, const CpuPoolFacts &, bool target_valid) noexcept;
    /// @brief Bound first-ever publication by one immutable activation timestamp.
    /// Resize clears per-revision first-frame facts but cannot reopen this clock.
    /// Stop/loss suppress new deadline failures; polling is never progress.
    [[nodiscard]] bool x11_first_frame_expired(std::optional<std::chrono::steady_clock::time_point> activation,
                                              std::chrono::steady_clock::time_point now, const CpuPoolFacts &) noexcept;
  }
  /// @brief Observe synchronized bytes and actual producer retirement separately.
  /// Quiescence certifies native owners and wake borrow have retired, not external
  /// frame leases or graphics use. made_progress follows accepted pool epochs and
  /// retirement; repeated polls do not extend a runtime drain deadline.
  struct X11CaptureProgress {
    detail::CpuPoolFacts frame;
    bool made_progress{}, quiescent{};
    std::optional<std::chrono::steady_clock::time_point> next_wake;
  };
  /// @brief Own an inactive checked connection, then one exclusive capture worker.
  /// create watches the frozen target before inspecting its current format. The
  /// factory must resolve the SAME catalog ID again and preserve generation/token
  /// before start; the worker also drains its own lifetime queue before admission.
  /// Main-thread progress/take/ack/Stop do bounded bookkeeping; byte conversion,
  /// allocation and live native I/O belong to the worker. wake runs on that worker
  /// and may only notify the host; its borrowed target must outlive quiescence.
  /// An inactive owner is safely destructible. After successful start the runtime
  /// must retain this owner until quiescent, or use its process-abandon policy.
  /// Destroying a live owner is a contract violation, never an implicit blocking
  /// join. Immutable frame leases can independently outlive producer retirement.
  class X11Capture final {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<X11Capture>, core::AppError> create(X11SourceTarget, std::function<void()> wake);
    ~X11Capture();
    X11Capture(const X11Capture &) = delete;
    X11Capture &operator=(const X11Capture &) = delete;
    [[nodiscard]] std::expected<void, core::AppError> start() noexcept;
    [[nodiscard]] X11CaptureProgress progress(std::chrono::steady_clock::time_point) noexcept;
    [[nodiscard]] std::optional<LinuxCapturedFrame> take_latest() noexcept;
    [[nodiscard]] std::expected<void, core::AppError> acknowledge_allocation_revision(std::uint64_t) noexcept;
    void request_stop() noexcept;
    [[nodiscard]] detail::CpuFormat initial_format() const noexcept;
    [[nodiscard]] std::uint64_t initial_revision() const noexcept;
  private:
    struct Impl;
    explicit X11Capture(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
  };
}
#endif
