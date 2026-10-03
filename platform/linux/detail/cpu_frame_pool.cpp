/// @file
/// @brief Native-independent writer admission and same-three-slot allocation barriers.
#include "platform/linux/detail/cpu_frame_pool.hpp"
#include <utility>
#include <limits>
namespace avb::linux_platform::detail {
  namespace {
    core::AppError error(core::AppErrorCode code) noexcept { return {code}; }
    bool mul(std::size_t a, std::size_t b, std::size_t &out) noexcept
    {
      if (a && b > std::numeric_limits<std::size_t>::max() / a) return false;
      out = a * b;
      return true;
    }
    void lose(CpuPoolState &s, core::AppError e) noexcept
    {
      if (s.facts.lost) return;
      s.facts.lost = true;
      s.facts.error = e;
      s.pending.reset();
      ++s.facts.epoch;
    }
    /// @brief Claim all three storage owners before image-sized reallocation.
    /// Stop, superseding negotiation and lease returns keep bounded access to facts
    /// while the worker frees/resizes bytes. Cancellation leaves storage unready;
    /// no writer can enter before this exact allocation obligation retires.
    void configure(CpuPoolState &s) noexcept
    {
      CpuFormat format;
      std::uint64_t revision{};
      std::size_t pitch{}, size{};
      {
        std::lock_guard lock{s.mutex};
        if (!s.facts.format || s.facts.stopping || s.facts.lost || s.storage_busy ||
            s.facts.storage_ready_revision == s.facts.revision) return;
        for (const auto &slot : s.slots)
          if (slot.leased || slot.writing) return;
        format = *s.facts.format;
        revision = s.facts.revision;
        if (!mul(format.allocation.width, 4, pitch) || !mul(pitch, format.allocation.height, size)) {
          lose(s, error(core::AppErrorCode::UnsupportedGraphics));
          return;
        }
        s.storage_busy = true;
      }
      bool cancelled = false, failed = false;
      try {
        // Retire all old allocations before allocating any new one: no replacement
        // pool or fourth owning byte buffer coexists with these same three slots.
        for (auto &slot : s.slots) std::vector<std::byte>{}.swap(slot.bytes);
        for (auto &slot : s.slots) {
          {
            std::lock_guard lock{s.mutex};
            cancelled = s.facts.stopping || s.facts.lost || s.facts.revision != revision;
          }
          if (cancelled) break;
          slot.bytes.resize(size);
        }
      } catch (...) {
        failed = true;
      }
      {
        std::lock_guard lock{s.mutex};
        s.storage_busy = false;
        if (s.facts.stopping || s.facts.lost || s.facts.revision != revision || cancelled) return;
        if (failed) {
          lose(s, error(core::AppErrorCode::RuntimeConstructionFailure));
          return;
        }
        s.pitch = pitch;
        s.facts.storage_ready_revision = revision;
        ++s.facts.epoch;
      }
    }
    bool can_ack(const CpuPoolState &s, std::uint64_t revision) noexcept
    {
      return revision && !s.facts.stopping && !s.facts.lost &&
             revision == s.facts.revision && revision == s.facts.storage_ready_revision;
    }
    bool can_write(const CpuPoolState &s, std::uint64_t revision) noexcept
    {
      return can_ack(s, revision) && !s.storage_busy &&
             (!s.facts.bound || s.admitted_revision == revision);
    }

  }    // namespace
  CpuFrameWrite::CpuFrameWrite(std::shared_ptr<CpuPoolState> pool, std::size_t slot, std::size_t pitch, std::uint64_t revision, core::Extent content, std::chrono::steady_clock::time_point observed) noexcept
      : pool_{std::move(pool)}, slot_{slot}, pitch_{pitch}, revision_{revision}, content_{content}, observed_{observed} {}
  CpuFrameWrite::~CpuFrameWrite() { cancel(); }
  CpuFrameWrite::CpuFrameWrite(CpuFrameWrite &&other) noexcept
      : pool_{std::move(other.pool_)}, slot_{other.slot_}, pitch_{other.pitch_}, revision_{other.revision_}, content_{other.content_}, observed_{other.observed_} {}
  CpuFrameWrite &CpuFrameWrite::operator=(CpuFrameWrite &&other) noexcept
  {
    if (this != &other) {
      cancel();
      pool_ = std::move(other.pool_);
      slot_ = other.slot_;
      pitch_ = other.pitch_;
      revision_ = other.revision_;
      content_ = other.content_;
      observed_ = other.observed_;
    }
    return *this;
  }
  void CpuFrameWrite::cancel() noexcept
  {
    if (!pool_) return;
    {
      std::lock_guard lock{pool_->mutex};
      pool_->slots[slot_].writing = false;
      ++pool_->facts.epoch;
    }
    pool_.reset();
  }
  std::span<std::byte> CpuFrameWrite::bytes() noexcept
  {
    return pool_ ? std::span<std::byte>{pool_->slots[slot_].bytes} : std::span<std::byte>{};
  }
  std::size_t CpuFrameWrite::row_pitch() const noexcept { return pool_ ? pitch_ : 0; }
  bool CpuFrameWrite::commit() noexcept
  {
    if (!pool_) return false;
    auto pool = std::move(pool_);
    std::lock_guard lock{pool->mutex};
    auto &s = *pool;
    auto &slot = s.slots[slot_];
    const bool admitted = slot.writing && !slot.leased && can_write(s, revision_);
    slot.writing = false;
    if (!admitted) {
      ++s.facts.epoch;
      return false;
    }
    if (s.facts.sequence == UINT64_MAX) {
      lose(s, error(core::AppErrorCode::RuntimeStateLost));
      return false;
    }
    slot.content = content_;
    slot.sequence = ++s.facts.sequence;
    slot.observed = observed_;
    s.pending = slot_;
    s.facts.first_frame_revision = revision_;
    s.facts.content = content_;
    ++s.facts.epoch;
    return true;
  }
  CpuFramePool::CpuFramePool() : state_{std::make_shared<CpuPoolState>()} {}
  std::expected<void, core::AppError> CpuFramePool::negotiate(CpuFormat format) noexcept
  {
    {
      std::lock_guard lock{state_->mutex};
      auto &s = *state_;
      if (s.facts.stopping || s.facts.lost) return std::unexpected(error(core::AppErrorCode::RuntimeStateLost));
      if (!format.allocation.width || !format.allocation.height) return std::unexpected(error(core::AppErrorCode::UnsupportedGraphics));
      if (s.facts.format && *s.facts.format == format) return {};
      const auto next = next_allocation_revision(s.facts.revision);
      if (!next) {
        lose(s, error(core::AppErrorCode::RuntimeStateLost));
        return std::unexpected(*s.facts.error);
      }
      s.pending.reset();
      s.facts.format = format;
      s.facts.revision = *next;
      s.facts.storage_ready_revision = 0;
      s.facts.first_frame_revision = 0;
      s.facts.content = {};
      ++s.facts.epoch;
    }
    configure(*state_);
    std::lock_guard lock{state_->mutex};
    if (state_->facts.lost) return std::unexpected(*state_->facts.error);
    return {};
  }
  std::optional<CpuFrameWrite> CpuFramePool::begin_write(std::uint64_t expected_revision,
                                                         core::Extent content,
                                                         std::chrono::steady_clock::time_point observed) noexcept
  {
    std::lock_guard lock{state_->mutex};
    auto &s = *state_;
    if (!can_write(s, expected_revision) || !s.facts.format || !content.width ||
        !content.height || content.width > s.facts.format->allocation.width ||
        content.height > s.facts.format->allocation.height) return {};
    for (const auto &slot : s.slots)
      if (slot.writing) return {};
    std::optional<std::size_t> selected;
    // Preserve the last pending frame during conversion whenever another slot is
    // available. Reuse pending only when the other two slots have external leases.
    for (std::size_t i = 0; i < s.slots.size(); ++i)
      if (!s.slots[i].leased && (!s.pending || *s.pending != i)) {
        selected = i;
        break;
      }
    if (!selected && s.pending && !s.slots[*s.pending].leased) {
      selected = s.pending;
      s.pending.reset();
    }
    if (!selected) return {};
    s.slots[*selected].writing = true;
    return CpuFrameWrite{state_, *selected, s.pitch, expected_revision, content, observed};
  }
  std::optional<LinuxCapturedFrame> CpuFramePool::take_latest() noexcept
  {
    std::lock_guard lock{state_->mutex};
    auto &s = *state_;
    if (!s.pending || s.facts.stopping || s.facts.lost) return {};
    const auto index = *s.pending;
    s.pending.reset();
    auto &slot = s.slots[index];
    slot.leased = true;
    return LinuxCapturedFrame{state_, index, s.pitch, slot.content, s.facts.format->allocation,
                              s.facts.revision, slot.sequence, slot.observed};
  }
  CpuPoolFacts CpuFramePool::facts() const noexcept
  {
    std::lock_guard lock{state_->mutex};
    return state_->facts;
  }
  std::expected<void, core::AppError> CpuFramePool::acknowledge(std::uint64_t revision) noexcept
  {
    std::lock_guard lock{state_->mutex};
    auto &s = *state_;
    if (!can_ack(s, revision)) return std::unexpected(error(core::AppErrorCode::RuntimeStateLost));
    if (!s.facts.bound || s.admitted_revision != revision) {
      s.facts.bound = true;
      s.admitted_revision = revision;
      ++s.facts.epoch;
    }
    return {};
  }
  CurrentFirstFrameBinding::CurrentFirstFrameBinding(std::shared_ptr<CpuPoolState> p, std::unique_lock<std::mutex> l) noexcept : pool_{std::move(p)}, lock_{std::move(l)} {}
  std::expected<CurrentFirstFrameBinding, core::AppError> CpuFramePool::bind_current_first_frame(std::uint64_t revision) noexcept
  {
    std::unique_lock lock{state_->mutex};
    auto &s = *state_;
    if (!can_ack(s, revision) || s.facts.first_frame_revision != revision)
      return std::unexpected(error(core::AppErrorCode::RuntimeStateLost));
    if (!s.facts.bound || s.admitted_revision != revision) {
      s.facts.bound = true;
      s.admitted_revision = revision;
      ++s.facts.epoch;
    }
    return CurrentFirstFrameBinding{state_, std::move(lock)};
  }
  void CpuFramePool::service_storage() noexcept { configure(*state_); }
  void CpuFramePool::request_stop() noexcept
  {
    std::lock_guard lock{state_->mutex};
    if (state_->facts.stopping) return;
    state_->facts.stopping = true;
    state_->pending.reset();
    ++state_->facts.epoch;
  }
  void CpuFramePool::fail(core::AppError e) noexcept
  {
    std::lock_guard lock{state_->mutex};
    lose(*state_, e);
  }
}    // namespace avb::linux_platform::detail
