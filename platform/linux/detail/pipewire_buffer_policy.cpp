/// @file
/// @brief Checked CPU metadata normalization and same-three-slot allocation barriers.
#include "platform/linux/detail/pipewire_buffer_policy.hpp"
#include <spa/buffer/meta.h>
#include <spa/buffer/buffer.h>
#include <algorithm>
#include <cstring>
#include <limits>
namespace avb::linux_platform::detail {
namespace {
core::AppError error(core::AppErrorCode code) noexcept { return {code}; }
bool add(std::size_t a,std::size_t b,std::size_t &out) noexcept {
  if (b>std::numeric_limits<std::size_t>::max()-a) return false;
  out=a+b; return true;
}
bool mul(std::size_t a,std::size_t b,std::size_t &out) noexcept {
  if (a && b>std::numeric_limits<std::size_t>::max()/a) return false;
  out=a*b; return true;
}
void lose(CpuPoolState &s,core::AppError e) noexcept {
  if (s.facts.lost) return;
  s.facts.lost=true; s.facts.error=e; s.pending.reset(); ++s.facts.epoch;
}
// Claim all three storage owners under brief bookkeeping. Allocation/freeing
// remains worker-exclusive outside the facts lock, so Stop and lease return do
// not wait for image-sized work. A superseding negotiation may change only facts
// until this allocation obligation has retired.
void configure(CpuPoolState &s) noexcept {
  CpuFormat format; std::uint64_t revision{}; std::size_t pitch{},size{};
  {
    std::lock_guard lock{s.mutex};
    if (!s.facts.format || s.facts.stopping || s.facts.lost || s.storage_busy ||
        s.facts.storage_ready_revision==s.facts.revision) return;
    for (const auto &slot:s.slots) if (slot.leased || slot.writing) return;
    format=*s.facts.format; revision=s.facts.revision;
    if (!mul(format.allocation.width,4,pitch) || !mul(pitch,format.allocation.height,size)) {
      lose(s,error(core::AppErrorCode::UnsupportedGraphics)); return;
    }
    s.storage_busy=true;
  }
  bool cancelled=false,failed=false;
  try {
    // Retire all old allocations before allocating any new one: no replacement
    // pool or fourth owning byte buffer coexists with these same three slots.
    for (auto &slot:s.slots) std::vector<std::byte>{}.swap(slot.bytes);
    for (auto &slot:s.slots) {
      {
        std::lock_guard lock{s.mutex};
        cancelled=s.facts.stopping || s.facts.lost || s.facts.revision!=revision;
      }
      if (cancelled) break;
      slot.bytes.resize(size);
    }
  } catch (...) { failed=true; }
  {
    std::lock_guard lock{s.mutex}; s.storage_busy=false;
    if (s.facts.stopping || s.facts.lost || s.facts.revision!=revision || cancelled) return;
    if (failed) { lose(s,error(core::AppErrorCode::RuntimeConstructionFailure)); return; }
    s.pitch=pitch; s.facts.storage_ready_revision=revision; ++s.facts.epoch;
  }
}
bool can_ack(const CpuPoolState &s,std::uint64_t revision) noexcept {
  return revision && !s.facts.stopping && !s.facts.lost &&
    revision==s.facts.revision && revision==s.facts.storage_ready_revision;
}
}
std::optional<CpuBufferPlan> inspect_cpu_buffer(const spa_buffer &buffer,
  CpuFormat format,std::span<const std::byte> mapping) noexcept {
  const auto a=format.allocation;
  if (!a.width || !a.height || buffer.n_datas!=1 || !buffer.datas ||
      (buffer.n_metas && !buffer.metas)) return {};
  const auto &data=buffer.datas[0];
  if ((data.type!=SPA_DATA_MemPtr && data.type!=SPA_DATA_MemFd) ||
      !(data.flags&SPA_DATA_FLAG_READABLE) || !data.chunk) return {};
  const auto &chunk=*data.chunk;
  if (chunk.flags&SPA_CHUNK_FLAG_CORRUPTED) return {};
  core::Extent content=a;
  std::size_t x=0,y=0;
  bool header_seen=false,crop_seen=false;
  for (std::uint32_t i=0;i<buffer.n_metas;++i) {
    const auto &meta=buffer.metas[i];
    if (meta.type==SPA_META_Header) {
      if (header_seen || !meta.data || meta.size<sizeof(spa_meta_header)) return {};
      header_seen=true;
      spa_meta_header header{}; std::memcpy(&header,meta.data,sizeof header);
      if (header.flags&(SPA_META_HEADER_FLAG_CORRUPTED|SPA_META_HEADER_FLAG_GAP)) return {};
    } else if (meta.type==SPA_META_VideoCrop) {
      if (crop_seen || !meta.data || meta.size<sizeof(spa_meta_region)) return {};
      crop_seen=true;
      spa_meta_region crop{}; std::memcpy(&crop,meta.data,sizeof crop);
      const auto &r=crop.region;
      // SPA marks a complete region with either zero dimension as no crop.
      // Envelope damage is checked above and cannot masquerade as that sentinel.
      if (!r.size.width || !r.size.height) continue;
      if (r.position.x<0 || r.position.y<0) return {};
      x=static_cast<std::size_t>(r.position.x); y=static_cast<std::size_t>(r.position.y);
      if (x>=a.width || y>=a.height || r.size.width>a.width-x || r.size.height>a.height-y) return {};
      content={r.size.width,r.size.height};
    }
  }
  CpuBufferPlan result{mapping,content,a,0,0,format.pixels,(chunk.flags&SPA_CHUNK_FLAG_EMPTY)!=0};
  if (result.neutral) { result.mapping={}; return result; }
  std::size_t full_row{},xbytes{},ystart{},start{},tail{},row{},end{},absolute{};
  if (!chunk.size || chunk.stride<=0 || !mul(a.width,4,full_row) ||
      static_cast<std::size_t>(chunk.stride)<full_row ||
      chunk.offset>data.maxsize || chunk.size>data.maxsize-chunk.offset ||
      !mul(x,4,xbytes) || !mul(y,chunk.stride,ystart) || !add(ystart,xbytes,start) ||
      !mul(content.height-1,chunk.stride,tail) || !mul(content.width,4,row) ||
      !add(start,tail,end) || !add(end,row,end) || end>chunk.size ||
      !add(chunk.offset,end,absolute) || absolute>mapping.size()) return {};
  result.source_offset=chunk.offset+start; result.source_pitch=chunk.stride;
  return result;
}
bool copy_cpu_buffer(const CpuBufferPlan &p,std::span<std::byte> destination,std::size_t pitch) noexcept {
  std::size_t row{},tail{},required{};
  if (!p.content.width || !p.content.height || p.content.width>p.allocation.width ||
      p.content.height>p.allocation.height || !mul(p.content.width,4,row) || pitch<row ||
      !mul(p.content.height-1,pitch,tail) || !add(tail,row,required) || required>destination.size()) return false;
  if (!p.neutral) {
    if (p.source_pitch<row || !mul(p.content.height-1,p.source_pitch,tail) ||
        !add(tail,row,required) || !add(p.source_offset,required,required) || required>p.mapping.size()) return false;
  }
  for (std::size_t y=0;y<p.content.height;++y) {
    auto *out=destination.data()+y*pitch;
    if (p.neutral) std::memset(out,0,row);
    else std::memcpy(out,p.mapping.data()+p.source_offset+y*p.source_pitch,row);
    if (p.neutral || p.pixels==CpuPixelFormat::Bgrx)
      for (std::size_t x=3;x<row;x+=4) out[x]=std::byte{255};
  }
  return true;
}
CpuFramePool::CpuFramePool() : state_{std::make_shared<CpuPoolState>()} {}
std::expected<void,core::AppError> CpuFramePool::negotiate(CpuFormat format) noexcept {
  {
    std::lock_guard lock{state_->mutex}; auto &s=*state_;
    if (s.facts.stopping || s.facts.lost) return std::unexpected(error(core::AppErrorCode::RuntimeStateLost));
    if (!format.allocation.width || !format.allocation.height) return std::unexpected(error(core::AppErrorCode::UnsupportedGraphics));
    if (s.facts.format && *s.facts.format==format) return {};
    const auto next=next_allocation_revision(s.facts.revision);
    if (!next) {
      lose(s,error(core::AppErrorCode::RuntimeStateLost)); return std::unexpected(*s.facts.error);
    }
    s.pending.reset(); s.facts.format=format; s.facts.revision=*next;
    s.facts.storage_ready_revision=0; s.facts.first_frame_revision=0; s.facts.content={}; ++s.facts.epoch;
  }
  configure(*state_);
  std::lock_guard lock{state_->mutex};
  if (state_->facts.lost) return std::unexpected(*state_->facts.error);
  return {};
}
bool CpuFramePool::ingest(const spa_buffer &buffer,std::span<const std::byte> mapping,
  std::chrono::steady_clock::time_point now,const std::function<int()> &requeue) noexcept {
  auto &s=*state_; configure(s);
  std::optional<CpuFormat> format; std::uint64_t revision{};
  {
    std::lock_guard lock{s.mutex};
    revision=s.facts.revision;
    if (!s.facts.stopping && !s.facts.lost && !s.storage_busy &&
        s.facts.storage_ready_revision==revision &&
        (!s.facts.bound || s.admitted_revision==revision)) format=s.facts.format;
  }
  // Metadata traversal also stays outside the bounded facts critical section.
  auto plan=format ? inspect_cpu_buffer(buffer,*format,mapping) : std::nullopt;
  std::optional<std::size_t> selected; std::size_t pitch{};
  if (plan) {
    std::lock_guard lock{s.mutex};
    bool other_writer=false;
    for (const auto &slot:s.slots) if (slot.writing) other_writer=true;
    if (!s.facts.stopping && !s.facts.lost && !s.storage_busy && !other_writer &&
        s.facts.revision==revision && s.facts.storage_ready_revision==revision &&
        (!s.facts.bound || s.admitted_revision==revision)) {
      // Prefer a free slot to keep the prior legal pending frame available while
      // copying. Only overwrite pending when the other two slots are leased.
      for (std::size_t i=0;i<s.slots.size();++i)
        if (!s.slots[i].leased && (!s.pending || *s.pending!=i)) { selected=i; break; }
      if (!selected && s.pending) { selected=s.pending; s.pending.reset(); }
      if (selected) { s.slots[*selected].writing=true; pitch=s.pitch; }
    }
  }
  const bool copied=selected && copy_cpu_buffer(*plan,s.slots[*selected].bytes,pitch);
  // This call owns exactly one native requeue, including malformed, full, Stop
  // and obsolete paths. Its reserved slot remains exclusive until the call ends,
  // but the main-facing mutex is free throughout copying and native work.
  const int queued=requeue();
  {
    std::lock_guard lock{s.mutex};
    if (selected) s.slots[*selected].writing=false;
    if (queued<0) { lose(s,error(core::AppErrorCode::PlatformOperationFailure)); return false; }
    if (!copied || s.facts.stopping || s.facts.lost || s.facts.revision!=revision ||
        s.facts.storage_ready_revision!=revision) return false;
    if (s.facts.sequence==UINT64_MAX) { lose(s,error(core::AppErrorCode::RuntimeStateLost)); return false; }
    auto &slot=s.slots[*selected]; slot.content=plan->content;
    slot.sequence=++s.facts.sequence; slot.observed=now;
    s.pending=selected; s.facts.first_frame_revision=revision; s.facts.content=plan->content; ++s.facts.epoch;
  }
  return true;
}
std::optional<LinuxCapturedFrame> CpuFramePool::take_latest() noexcept {
  std::lock_guard lock{state_->mutex}; auto &s=*state_;
  if (!s.pending || s.facts.stopping || s.facts.lost) return {};
  const auto index=*s.pending; s.pending.reset(); auto &slot=s.slots[index]; slot.leased=true;
  return LinuxCapturedFrame{state_,index,s.pitch,slot.content,s.facts.format->allocation,
    s.facts.revision,slot.sequence,slot.observed};
}
CpuPoolFacts CpuFramePool::facts() const noexcept { std::lock_guard lock{state_->mutex}; return state_->facts; }
std::expected<void,core::AppError> CpuFramePool::acknowledge(std::uint64_t revision) noexcept {
  std::lock_guard lock{state_->mutex}; auto &s=*state_;
  if (!can_ack(s,revision)) return std::unexpected(error(core::AppErrorCode::RuntimeStateLost));
  if (!s.facts.bound || s.admitted_revision!=revision) {
    s.facts.bound=true; s.admitted_revision=revision; ++s.facts.epoch;
  }
  return {};
}
CurrentFirstFrameBinding::CurrentFirstFrameBinding(std::shared_ptr<CpuPoolState> p,std::unique_lock<std::mutex> l) noexcept : pool_{std::move(p)},lock_{std::move(l)} {}
std::expected<CurrentFirstFrameBinding,core::AppError> CpuFramePool::bind_current_first_frame(std::uint64_t revision) noexcept {
  std::unique_lock lock{state_->mutex}; auto &s=*state_;
  if (!can_ack(s,revision) || s.facts.first_frame_revision!=revision)
    return std::unexpected(error(core::AppErrorCode::RuntimeStateLost));
  if (!s.facts.bound || s.admitted_revision!=revision) {
    s.facts.bound=true; s.admitted_revision=revision; ++s.facts.epoch;
  }
  return CurrentFirstFrameBinding{state_,std::move(lock)};
}
void CpuFramePool::service_storage() noexcept { configure(*state_); }
void CpuFramePool::request_stop() noexcept {
  std::lock_guard lock{state_->mutex};
  if (state_->facts.stopping) return;
  state_->facts.stopping=true; state_->pending.reset(); ++state_->facts.epoch;
}
void CpuFramePool::fail(core::AppError e) noexcept { std::lock_guard lock{state_->mutex}; lose(*state_,e); }
}
