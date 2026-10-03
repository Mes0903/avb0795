/// @file
/// @brief Native-independent three-slot BGRA storage, revision admission and leases.
#ifndef AVB0795_LINUX_CPU_FRAME_POOL_HPP
#define AVB0795_LINUX_CPU_FRAME_POOL_HPP

#include "platform/linux/linux_captured_frame.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace avb::linux_platform::detail {
  /// @brief Issue a nonzero allocation identity without permitting wraparound reuse.
  [[nodiscard]] constexpr std::optional<std::uint64_t> next_allocation_revision(std::uint64_t previous) noexcept
  {
    if (previous == UINT64_MAX) return {};
    return previous + 1;
  }

  enum class CpuPixelFormat { Bgra,
                              Bgrx };

  /// @brief Negotiated source identity; every published slot contains BGRA bytes.
  /// Pixel format participates in revision changes because native conversion may
  /// change even at the same extent. Per-frame content cropping is a separate fact.
  struct CpuFormat {
    CpuPixelFormat pixels{CpuPixelFormat::Bgra};
    core::Extent allocation{};
    friend bool operator==(const CpuFormat &, const CpuFormat &) = default;
  };

  /// @brief Bounded observations read under the same mutex as admission and Stop.
  /// Current revision may precede storage readiness while old leases remain held.
  /// First-frame revision certifies actual publication, independently of ring
  /// acknowledgment; epoch changes represent ownership or accepted-state changes.
  struct CpuPoolFacts {
    std::optional<CpuFormat> format;
    core::Extent content{};
    std::uint64_t revision{}, storage_ready_revision{}, first_frame_revision{}, sequence{}, epoch{};
    bool bound{}, stopping{}, lost{};
    std::optional<core::AppError> error;
  };

  /// @brief Shared storage retained by writers and immutable external frame leases.
  /// The mutex protects phase/revision facts, while a writer exclusively owns its
  /// slot's bytes outside the lock. Allocation claims all three only after every
  /// writer and reader returns. Pending names one slot; it never owns extra bytes.
  /// This internal state contains no native envelopes, queue callbacks or graphics.
  struct CpuPoolState {
    struct Slot {
      std::vector<std::byte> bytes;
      bool leased{}, writing{};
      core::Extent content{};
      std::uint64_t sequence{};
      std::chrono::steady_clock::time_point observed{};
    };
    mutable std::mutex mutex;
    std::array<Slot, 3> slots;
    CpuPoolFacts facts;
    std::optional<std::size_t> pending;
    std::size_t pitch{};
    std::uint64_t admitted_revision{};
    bool storage_busy{};
  };

  /// @brief Reserve one exclusive slot without borrowing a native image payload.
  /// Move transfers the reservation. Only its producer thread accesses the mutable
  /// bytes; those borrows end before commit, destruction or move assignment. Native
  /// inspection/conversion and native reply retirement occur outside the pool lock
  /// and finish before commit. Commit consumes this writer on either outcome and
  /// rechecks the exact revision, bound-ring admission and Stop/loss gates before
  /// publishing once. Destruction cancels an uncommitted writer. A cancelled or
  /// moved-from writer exposes no bytes and cannot publish. Returning the slot
  /// acknowledges byte-work retirement; it does not certify a native worker's exit.
  class CpuFrameWrite final {
  public:
    ~CpuFrameWrite();
    CpuFrameWrite(const CpuFrameWrite &) = delete;
    CpuFrameWrite &operator=(const CpuFrameWrite &) = delete;
    CpuFrameWrite(CpuFrameWrite &&) noexcept;
    CpuFrameWrite &operator=(CpuFrameWrite &&) noexcept;
    [[nodiscard]] std::span<std::byte> bytes() noexcept;
    [[nodiscard]] std::size_t row_pitch() const noexcept;
    [[nodiscard]] bool commit() noexcept;

  private:
    CpuFrameWrite(std::shared_ptr<CpuPoolState>, std::size_t, std::size_t, std::uint64_t, core::Extent, std::chrono::steady_clock::time_point) noexcept;
    void cancel() noexcept;
    std::shared_ptr<CpuPoolState> pool_;
    std::size_t slot_{}, pitch_{};
    std::uint64_t revision_{};
    core::Extent content_{};
    std::chrono::steady_clock::time_point observed_{};
    friend class CpuFramePool;
  };

  /// @brief Hold validated first-frame binding through a short owner transfer.
  /// Historical registry callers acquire their registry lock first. This guard
  /// retains the pool and facts lock through only noexcept attachment; release it
  /// on the acquiring thread before any other pool or frame-lease operation.
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

  /// @brief Own exactly three reusable slots and newest-only publication policy.
  /// Negotiation closes admission for the previous allocation; worker-side storage
  /// service waits for leases/writers and resizes the same three slots outside the
  /// facts lock. It releases all previous byte allocations before creating new ones.
  /// Image-sized negotiate/service_storage work belongs to the producer thread
  /// or inactive construction; a live main tick uses only bounded observations.
  /// begin_write performs only bounded bookkeeping and rejects zero/stale/not-ready
  /// revisions, invalid content, Stop/loss and overlapping writer/reader ownership.
  /// Initial unbound publication may precede renderer acknowledgment; once bound,
  /// reservation and commit require acknowledgment of this exact current revision.
  /// Main-thread facts/take/acknowledge/Stop and frame returns never wait for native
  /// image conversion or image-sized allocation. Native intake owns that work and
  /// retains its worker until retirement; this pool has no native queue authority.
  class CpuFramePool final {
  public:
    CpuFramePool();
    CpuFramePool(const CpuFramePool &) = delete;
    CpuFramePool &operator=(const CpuFramePool &) = delete;
    [[nodiscard]] std::expected<void, core::AppError> negotiate(CpuFormat) noexcept;
    [[nodiscard]] std::optional<CpuFrameWrite> begin_write(std::uint64_t expected_revision,
                                                           core::Extent content,
                                                           std::chrono::steady_clock::time_point observed) noexcept;
    [[nodiscard]] std::optional<LinuxCapturedFrame> take_latest() noexcept;
    [[nodiscard]] CpuPoolFacts facts() const noexcept;

    /// @brief Admit an installed ring's exact current, storage-ready allocation.
    /// Zero/stale/not-ready revisions and Stop/loss leave admission unchanged.
    /// Repetition is idempotent. Acknowledgment may precede the first legal frame;
    /// it establishes ring admission, not actual capture or presentation readiness.
    [[nodiscard]] std::expected<void, core::AppError> acknowledge(std::uint64_t) noexcept;

    /// @brief Bind only a published current first frame, retaining the facts lock.
    /// Failure preserves ownership and admission. Success returns the guard for
    /// historical noexcept prepared-owner transfer. The first-frame condition is
    /// stricter than acknowledge and must not be bypassed by equal dimensions.
    [[nodiscard]] std::expected<CurrentFirstFrameBinding, core::AppError> bind_current_first_frame(std::uint64_t) noexcept;
    void service_storage() noexcept;
    void request_stop() noexcept;
    void fail(core::AppError) noexcept;

  private:
    std::shared_ptr<CpuPoolState> state_;
  };
}    // namespace avb::linux_platform::detail
#endif
