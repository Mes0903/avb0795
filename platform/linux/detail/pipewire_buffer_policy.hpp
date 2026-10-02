/// @file
/// @brief Checked SPA CPU layout and bounded frame publication policy.
#ifndef AVB0795_LINUX_PIPEWIRE_BUFFER_POLICY_HPP
#define AVB0795_LINUX_PIPEWIRE_BUFFER_POLICY_HPP
#include "platform/linux/linux_captured_frame.hpp"
#include <array>
#include <vector>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
struct spa_buffer;
namespace avb::linux_platform::detail {
/// @brief Issue a nonzero owner-scoped revision without wrapping identity.
[[nodiscard]] constexpr std::optional<std::uint64_t> next_allocation_revision(std::uint64_t previous) noexcept {
  if (previous==UINT64_MAX) return {};
  return previous+1;
}
enum class CpuPixelFormat { Bgra, Bgrx };
/// @brief Immutable negotiated identity; content cropping is a per-frame fact.
struct CpuFormat {
  CpuPixelFormat pixels{CpuPixelFormat::Bgra};
  core::Extent allocation{};
  friend bool operator==(const CpuFormat &, const CpuFormat &) = default;
};
/// @brief Validated source plan into the data-memory base, never the FD origin.
/// EMPTY plans do not borrow pixels. Ordinary plans borrow the mapping through
/// copy_cpu_buffer only; wraparound payloads and unsupported memory are rejected.
struct CpuBufferPlan {
  std::span<const std::byte> mapping;
  core::Extent content{}, allocation{};
  std::size_t source_offset{}, source_pitch{};
  CpuPixelFormat pixels{};
  bool neutral{};
};
/// @brief Validate one borrowed native envelope against negotiated CPU format.
/// mapping starts at the already-adjusted data-memory origin, not the FD's map
/// offset. Keep envelope/metadata readable during inspection and keep an ordinary
/// returned plan's pixels stable through copy; no envelope references are retained.
/// Missing crop or a complete zero-dimension sentinel means full extent. Damaged
/// metadata/corruption/GAP reject; legal EMPTY returns neutral without reading pixels.
/// Returns nullopt for unsupported or unsafe layout and never writes source bytes.
[[nodiscard]] std::optional<CpuBufferPlan> inspect_cpu_buffer(
  const spa_buffer &, CpuFormat, std::span<const std::byte>) noexcept;
/// @brief Copy validated content to destination origin using its supplied pitch.
/// Both disjoint spans stay live/stable through this synchronous worker call.
/// BGRx and neutral content become opaque BGRA; pixels outside content are untouched.
/// All range/arithmetic checks precede writes: false preserves the destination.
/// No mapping, native requeue, publication or allocation is performed here.
[[nodiscard]] bool copy_cpu_buffer(const CpuBufferPlan &, std::span<std::byte>, std::size_t) noexcept;
/// @brief Latest bounded facts under the same mutex as publication and Stop.
struct CpuPoolFacts {
  std::optional<CpuFormat> format;
  core::Extent content{};
  std::uint64_t revision{}, storage_ready_revision{}, first_frame_revision{}, sequence{}, epoch{};
  bool bound{}, stopping{}, lost{};
  std::optional<core::AppError> error;
};
/// @brief Shared storage outliving its owner through immutable external leases.
/// Mutex protects all slot phase changes and revision facts. A leased slot's bytes
/// are never touched until its frame returns. A writer owns its slot through copy
/// and requeue; allocation owns all three only after every lease/writer returns.
/// Heavy byte work never holds the facts mutex.
struct CpuPoolState {
  struct Slot { std::vector<std::byte> bytes; bool leased{}, writing{}; core::Extent content{};
    std::uint64_t sequence{}; std::chrono::steady_clock::time_point observed{}; };
  mutable std::mutex mutex;
  std::array<Slot,3> slots;
  CpuPoolFacts facts;
  std::optional<std::size_t> pending;
  std::size_t pitch{};
  std::uint64_t admitted_revision{};
  bool storage_busy{};
};
/// @brief Hold current-first-frame validation and binding through owner transfer.
/// The registry holds its own lock before acquiring this guard. Producer callbacks
/// never take registry locks. Move/attachment during this guard must be noexcept;
/// release the guard on the same thread before any other producer or frame-lease operation.
class CurrentFirstFrameBinding final {
public:
  CurrentFirstFrameBinding(CurrentFirstFrameBinding &&) noexcept = default;
  CurrentFirstFrameBinding &operator=(CurrentFirstFrameBinding &&) = delete;
private:
  explicit CurrentFirstFrameBinding(std::shared_ptr<CpuPoolState>, std::unique_lock<std::mutex>) noexcept;
  std::shared_ptr<CpuPoolState> pool_;
  std::unique_lock<std::mutex> lock_;
  friend class CpuFramePool;
};
/// @brief Concrete three-slot owner shared by native intake and external leases.
/// Negotiation closes intake and waits for every lease before resizing the same
/// three slots. Intake copies, requeues exactly once, then publishes. No per-frame
/// byte allocation occurs. Queue callbacks are synchronous native queue authority,
/// never retained. Inspection/copy/allocation/requeue run outside the facts mutex
/// under exclusive slot/storage reservations; Stop/facts/take and frame returns
/// perform bounded bookkeeping and do not wait for that native work. Requeue must
/// not throw or recursively initiate producer byte work. Worker methods may do image-sized
/// work; their owner must remain retained until they finish. Initial preparation is
/// unbound. The short final-binding guard intentionally serializes owner transfer.
class CpuFramePool final {
public:
  CpuFramePool();
  [[nodiscard]] std::expected<void, core::AppError> negotiate(CpuFormat) noexcept;
  [[nodiscard]] bool ingest(const spa_buffer &, std::span<const std::byte>,
    std::chrono::steady_clock::time_point, const std::function<int()> &requeue) noexcept;
  [[nodiscard]] std::optional<LinuxCapturedFrame> take_latest() noexcept;
  [[nodiscard]] CpuPoolFacts facts() const noexcept;
  /// @brief Bind/open intake for exactly the current storage-ready revision.
  /// Main-thread admission operation; it can succeed before any legal first frame.
  /// Zero/stale/superseded/not-ready revisions and Stop/loss reject without opening.
  /// Repeating a successful acknowledgment is idempotent. This supplies bound ring
  /// replacement admission, not the initial prepared-owner transfer proof.
  [[nodiscard]] std::expected<void, core::AppError> acknowledge(std::uint64_t) noexcept;
  /// @brief Jointly validate current legal first-frame readiness and bind its revision.
  /// Main-thread registry commit acquires its registry lock first. Failure preserves
  /// owner/admission; success returns the held pool guard spanning noexcept transfer
  /// and attachment. Release it on this thread before any other pool/lease operation.
  /// Unlike acknowledge, this rejects a revision with no accepted current first frame;
  /// both reject Stop/loss and stale, zero or not-storage-ready revisions.
  [[nodiscard]] std::expected<CurrentFirstFrameBinding, core::AppError> bind_current_first_frame(std::uint64_t) noexcept;
  void service_storage() noexcept;
  void request_stop() noexcept;
  void fail(core::AppError) noexcept;
private:
  std::shared_ptr<CpuPoolState> state_;
};
}
#endif
