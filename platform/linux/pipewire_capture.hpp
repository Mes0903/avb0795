/// @file
/// @brief Inactive native capture ownership and asynchronous CPU stream lifecycle.
#ifndef AVB0795_LINUX_PIPEWIRE_CAPTURE_HPP
#define AVB0795_LINUX_PIPEWIRE_CAPTURE_HPP
#include "platform/linux/portal_screencast.hpp"
#include "platform/linux/detail/pipewire_buffer_policy.hpp"
struct pw_context;
struct pw_core;
struct pw_properties;
namespace avb::linux_platform {
namespace detail {
/// @brief Connect using the audited native/SPA ownership contract.
/// Properties are consumed on both outcomes. FD stays local through NULL failure
/// and transfers only for nonnull core. Dependency/protocol/loop replacements must
/// revalidate this contract; the header's universal error-close claim differs.
[[nodiscard]] pw_core *connect_owned_remote(pw_context *, PortalUniqueFd &, pw_properties *) noexcept;
struct PipeWireWorker;
}
/// @brief Bounded current revision facts plus actual native retirement observations.
/// quiescent covers callbacks, worker, native owners and wake borrows, independently
/// of external CPU leases. made_progress comes from acknowledgments/publication.
struct PipeWireProgress {
  detail::CpuPoolFacts frame;
  bool made_progress{}, quiescent{};
  std::optional<std::chrono::steady_clock::time_point> next_wake;
};
/// @brief Retain this inactive owner before requesting activation.
/// Callbacks execute only on its private worker and never enter presentation code.
/// start failure leaves the complete owner available for asynchronous retirement.
/// Stop closes admission without joining. Keep the wake target alive and poll until
/// progress reports quiescent before destruction; live-worker destruction terminates.
/// A taken frame outlives native retirement. Methods are main-thread operations,
/// except request_stop may race intake; the concrete binding guard serializes final
/// prepared-owner transfer with negotiation/Stop and must end before other calls.
class PipeWireCapture final {
public:
  [[nodiscard]] static std::expected<std::unique_ptr<PipeWireCapture>,core::AppError>
  create(PortalStreamConnection, std::function<void()> wake);
  ~PipeWireCapture();
  PipeWireCapture(const PipeWireCapture &) = delete;
  PipeWireCapture &operator=(const PipeWireCapture &) = delete;
  [[nodiscard]] std::expected<void,core::AppError> start() noexcept;
  [[nodiscard]] PipeWireProgress progress(std::chrono::steady_clock::time_point) noexcept;
  [[nodiscard]] std::optional<LinuxCapturedFrame> take_latest() noexcept;
  /// @brief Main-thread admission acknowledgment after publishing a matching ring.
  /// Requires the exact current nonzero storage-ready revision and no Stop/loss.
  /// It may succeed before a legal first frame and is not an initial prepared-owner
  /// commit check. Same-revision repetition preserves state/progress; rejection
  /// preserves admission. Use bind_current_first_frame for the initial transfer.
  [[nodiscard]] std::expected<void,core::AppError> acknowledge_allocation_revision(std::uint64_t) noexcept;
  /// @brief Validate current legal first frame and matching revision under one lock.
  /// Main-thread final commit holds its registry lock before calling; failure keeps
  /// capture with that registry. The successful concrete guard spans only noexcept
  /// owner transfer/attachment and must unlock on this thread before further calls.
  /// Stop/loss, missing current first-frame readiness or stale/not-ready/zero revision
  /// reject. No fallible acknowledgment may follow successful capture transfer.
  [[nodiscard]] std::expected<detail::CurrentFirstFrameBinding,core::AppError>
  bind_current_first_frame(std::uint64_t) noexcept;
  void request_stop() noexcept;
private:
  explicit PipeWireCapture(std::unique_ptr<detail::PipeWireWorker>) noexcept;
  std::unique_ptr<detail::PipeWireWorker> worker_;
  std::uint64_t observed_epoch_{};
  bool observed_quiescent_{};
};
}
#endif
