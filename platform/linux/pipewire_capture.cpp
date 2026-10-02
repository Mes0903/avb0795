/// @file
/// @brief Private-loop PipeWire activation, callback publication and explicit retirement.
#include "platform/linux/pipewire_capture.hpp"
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/buffer/meta.h>
#include <spa/pod/builder.h>
#include <atomic>
#include <cerrno>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <sys/stat.h>
namespace avb::linux_platform {
namespace detail {
pw_core *connect_owned_remote(pw_context *context,PortalUniqueFd &fd,pw_properties *properties) noexcept {
  // core.h promises closure on error too. Audited standard native/SPA paths in
  // 0.3.48/1.0.5/1.2.0/1.4.2/1.6.2 retain the descriptor on synchronous NULL;
  // only nonnull installs native close authority. Revalidate replacement dependencies.
  // Properties are independently consumed on both outcomes; do not probe a failed FD.
  auto *core=pw_context_connect_fd(context,fd.get(),properties,0);
  if (core) (void)fd.release();
  return core;
}
/// @brief Single native callback authority retained until explicit worker acknowledgment.
/// Native pointers are worker-only except the guarded event used for nonblocking
/// stop signaling. Pool locks never cover native-loop locks or registry operations.
struct PipeWireWorker {
  PortalStreamConnection connection;
  std::function<void()> wake;
  CpuFramePool pool;
  std::thread thread;
  std::atomic<bool> stopping{},finished{true};
  bool started{};
  std::mutex event_mutex;
  pw_loop *loop{};
  spa_source *stop_event{};
  pw_stream *stream{};
  pw_core *core{};
  spa_hook stream_hook{},core_hook{};
  explicit PipeWireWorker(PortalStreamConnection c,std::function<void()> w) : connection{std::move(c)},wake{std::move(w)} {}
  void notify() noexcept {
    try { if (wake) wake(); }
    catch (...) { pool.fail({core::AppErrorCode::PlatformOperationFailure}); stopping=true; }
  }
  void fail(core::AppErrorCode code,int detail=0) noexcept {
    pool.fail({code,detail ? std::optional<std::int64_t>{detail} : std::nullopt}); notify();
  }
  static void on_error(void *data,std::uint32_t,int,int result,const char *) noexcept {
    auto &self=*static_cast<PipeWireWorker *>(data);
    self.fail(core::AppErrorCode::SourceUnavailable,result);
  }
  static void on_state(void *data,pw_stream_state old,pw_stream_state state,const char *) noexcept {
    auto &self=*static_cast<PipeWireWorker *>(data);
    if (state==PW_STREAM_STATE_ERROR || (state==PW_STREAM_STATE_UNCONNECTED && old!=PW_STREAM_STATE_UNCONNECTED && !self.stopping))
      self.fail(core::AppErrorCode::SourceUnavailable,errno);
  }
  static void on_format(void *data,std::uint32_t id,const spa_pod *param) noexcept {
    auto &self=*static_cast<PipeWireWorker *>(data);
    if (id!=SPA_PARAM_Format) return;
    if (!param) { self.fail(core::AppErrorCode::UnsupportedGraphics); return; }
    std::uint32_t media{},subtype{}; spa_video_info_raw info{};
    if (spa_format_parse(param,&media,&subtype)<0 || media!=SPA_MEDIA_TYPE_video || subtype!=SPA_MEDIA_SUBTYPE_raw ||
        spa_format_video_raw_parse(param,&info)<0 ||
        (info.format!=SPA_VIDEO_FORMAT_BGRA && info.format!=SPA_VIDEO_FORMAT_BGRx) ||
        (info.flags&(SPA_VIDEO_FLAG_MODIFIER|SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED)) ||
        !info.size.width || !info.size.height || info.size.width>INT32_MAX/4 ||
        info.size.height>static_cast<unsigned>(INT32_MAX)/(info.size.width*4)) {
      self.fail(core::AppErrorCode::UnsupportedGraphics); return;
    }
    auto result=self.pool.negotiate({info.format==SPA_VIDEO_FORMAT_BGRA ? CpuPixelFormat::Bgra : CpuPixelFormat::Bgrx,
      {info.size.width,info.size.height}});
    if (!result) { self.notify(); return; }
    std::byte bytes[1024]; spa_pod_builder builder=SPA_POD_BUILDER_INIT(bytes,sizeof bytes);
    const spa_pod *params[3];
    params[0]=static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,SPA_TYPE_OBJECT_ParamBuffers,SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers,SPA_POD_CHOICE_RANGE_Int(8,2,16),
      SPA_PARAM_BUFFERS_blocks,SPA_POD_Int(1),
      SPA_PARAM_BUFFERS_size,SPA_POD_Int(static_cast<int>(info.size.width*info.size.height*4)),
      SPA_PARAM_BUFFERS_stride,SPA_POD_Int(static_cast<int>(info.size.width*4)),
      SPA_PARAM_BUFFERS_dataType,SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemPtr)|(1<<SPA_DATA_MemFd))));
    params[1]=static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,SPA_TYPE_OBJECT_ParamMeta,SPA_PARAM_Meta,
      SPA_PARAM_META_type,SPA_POD_Id(SPA_META_Header),SPA_PARAM_META_size,SPA_POD_Int(sizeof(spa_meta_header))));
    params[2]=static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,SPA_TYPE_OBJECT_ParamMeta,SPA_PARAM_Meta,
      SPA_PARAM_META_type,SPA_POD_Id(SPA_META_VideoCrop),SPA_PARAM_META_size,SPA_POD_Int(sizeof(spa_meta_region))));
    const int updated=pw_stream_update_params(self.stream,params,3);
    if (updated<0) self.fail(core::AppErrorCode::RuntimeConstructionFailure,updated);
    else self.notify();
  }
  static void on_process(void *data) noexcept {
    auto &self=*static_cast<PipeWireWorker *>(data);
    while (auto *buffer=pw_stream_dequeue_buffer(self.stream)) {
      spa_buffer empty{};
      auto &source=buffer->buffer ? *buffer->buffer : empty;
      std::span<const std::byte> mapping;
      if (source.n_datas==1 && source.datas) {
        const auto &d=source.datas[0];
        const bool neutral=d.chunk && (d.chunk->flags&SPA_CHUNK_FLAG_EMPTY);
        bool mapped=d.data && d.maxsize &&
          reinterpret_cast<std::uintptr_t>(d.data)<=UINTPTR_MAX-d.maxsize;
        if (mapped && d.type==SPA_DATA_MemFd && !neutral) {
          // MAP_BUFFERS has already adjusted data to the logical data-memory
          // origin. Check backing length; adding mapoffset to data again is wrong.
          struct stat backing{};
          mapped=d.fd>=0 && d.fd<=INT_MAX && fstat(static_cast<int>(d.fd),&backing)==0 &&
            backing.st_size>=0 && static_cast<std::uint64_t>(d.mapoffset)+d.maxsize<=static_cast<std::uint64_t>(backing.st_size);
        }
        if (mapped && !neutral) mapping={static_cast<const std::byte *>(d.data),d.maxsize};
      }
      const auto before=self.pool.facts().epoch;
      const bool accepted=self.pool.ingest(source,mapping,std::chrono::steady_clock::now(),[&] { return pw_stream_queue_buffer(self.stream,buffer); });
      if (accepted || self.pool.facts().epoch!=before) self.notify();
    }
  }
  void run() noexcept {
    pw_context *context=nullptr;
    bool initialized=false,entered=false;
    try {
      if (!connection.session.valid()) fail(core::AppErrorCode::SourceUnavailable);
      else if (!stopping) {
        // The library owns its balanced process reference count. Retain one
        // reference through this complete context/loop retirement, without a singleton.
        pw_init(nullptr,nullptr); initialized=true;
        auto *created_loop=pw_loop_new(nullptr);
        { std::lock_guard lock{event_mutex}; loop=created_loop; }
        if (!loop) fail(core::AppErrorCode::RuntimeConstructionFailure,errno);
        else {
          pw_loop_enter(loop); entered=true;
          auto *created_event=pw_loop_add_event(loop,[](void *,std::uint64_t) {},nullptr);
          { std::lock_guard lock{event_mutex}; stop_event=created_event; }
          if (!stop_event) { fail(core::AppErrorCode::RuntimeConstructionFailure,errno); stopping=true; }
          auto *props=pw_properties_new(PW_KEY_CONFIG_NAME,"client.conf","context.num-data-loops","0","context.data-loops","[]","support.dbus","false",nullptr);
          if (!props) fail(core::AppErrorCode::RuntimeConstructionFailure,ENOMEM);
          else context=pw_context_new(loop,props,0);
          if (!context) fail(core::AppErrorCode::RuntimeConstructionFailure,errno);
          else if (!stopping) {
            auto *remote=pw_properties_new(PW_KEY_PROTOCOL,"PipeWire:Protocol:Native",nullptr);
            if (!remote) fail(core::AppErrorCode::RuntimeConstructionFailure,ENOMEM);
            else core=connect_owned_remote(context,connection.fd,remote);
            if (!core) fail(core::AppErrorCode::RuntimeConstructionFailure,errno);
            else {
              static const pw_core_events events=[] { pw_core_events e{}; e.version=PW_VERSION_CORE_EVENTS; e.error=on_error; return e; }();
              pw_core_add_listener(core,&core_hook,&events,this);
              const auto target=connection.serial ? std::to_string(*connection.serial) : std::to_string(connection.node_id);
              auto *stream_props=pw_properties_new(PW_KEY_MEDIA_TYPE,"Video",PW_KEY_MEDIA_CATEGORY,"Capture",
                PW_KEY_MEDIA_ROLE,"Screen",PW_KEY_NODE_NAME,"avb0795-capture",PW_KEY_TARGET_OBJECT,target.c_str(),nullptr);
              if (stream_props) stream=pw_stream_new(core,"avb0795 CPU capture",stream_props);
              if (!stream) fail(core::AppErrorCode::RuntimeConstructionFailure,errno);
              else {
                static const pw_stream_events stream_events=[] { pw_stream_events e{}; e.version=PW_VERSION_STREAM_EVENTS;
                  e.state_changed=on_state; e.param_changed=on_format; e.process=on_process; return e; }();
                pw_stream_add_listener(stream,&stream_hook,&stream_events,this);
                std::byte bytes[1024]; spa_pod_builder b=SPA_POD_BUILDER_INIT(bytes,sizeof bytes);
                const spa_rectangle size{320,240},minimum{1,1},maximum{16384,16384};
                const spa_fraction rate{30,1},min_rate{0,1},max_rate{1000,1};
                const spa_pod *format=static_cast<spa_pod *>(spa_pod_builder_add_object(&b,SPA_TYPE_OBJECT_Format,SPA_PARAM_EnumFormat,
                  SPA_FORMAT_mediaType,SPA_POD_Id(SPA_MEDIA_TYPE_video),SPA_FORMAT_mediaSubtype,SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                  SPA_FORMAT_VIDEO_format,SPA_POD_CHOICE_ENUM_Id(3,SPA_VIDEO_FORMAT_BGRA,SPA_VIDEO_FORMAT_BGRA,SPA_VIDEO_FORMAT_BGRx),
                  SPA_FORMAT_VIDEO_size,SPA_POD_CHOICE_RANGE_Rectangle(&size,&minimum,&maximum),
                  SPA_FORMAT_VIDEO_framerate,SPA_POD_CHOICE_RANGE_Fraction(&rate,&min_rate,&max_rate)));
                const int result=pw_stream_connect(stream,PW_DIRECTION_INPUT,PW_ID_ANY,
                  static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT|PW_STREAM_FLAG_MAP_BUFFERS),&format,1);
                if (result<0) fail(core::AppErrorCode::RuntimeConstructionFailure,result);
                else while (!stopping && !pool.facts().lost) {
                  if (!connection.session.valid()) { fail(core::AppErrorCode::SourceUnavailable); break; }
                  const int iterated=pw_loop_iterate(loop,50);
                  if (iterated<0 && iterated!=-EINTR) { fail(core::AppErrorCode::PlatformOperationFailure,iterated); break; }
                  pool.service_storage();
                }
              }
            }
          }
        }
      }
    } catch (...) { fail(core::AppErrorCode::RuntimeConstructionFailure); }
    pool.request_stop();
    if (stream) { spa_hook_remove(&stream_hook); pw_stream_destroy(stream); stream=nullptr; }
    if (core) { spa_hook_remove(&core_hook); pw_core_disconnect(core); core=nullptr; }
    if (context) pw_context_destroy(context);
    pw_loop *retired_loop; spa_source *retired_event;
    { std::lock_guard lock{event_mutex};
      retired_loop=std::exchange(loop,nullptr); retired_event=std::exchange(stop_event,nullptr);
    }
    if (retired_event) pw_loop_destroy_source(retired_loop,retired_event);
    if (entered) pw_loop_leave(retired_loop);
    if (retired_loop) pw_loop_destroy(retired_loop);
    connection.fd=PortalUniqueFd{}; connection.session={};
    if (initialized) pw_deinit();
    // No future callback, native owner or wake borrow survives this acknowledgment.
    wake={}; finished.store(true,std::memory_order_release);
  }
};
}
PipeWireCapture::PipeWireCapture(std::unique_ptr<detail::PipeWireWorker> w) noexcept : worker_{std::move(w)} {}
PipeWireCapture::~PipeWireCapture() {
  if (!worker_->finished.load(std::memory_order_acquire)) std::terminate();
  if (worker_->thread.joinable()) worker_->thread.join();
}
std::expected<std::unique_ptr<PipeWireCapture>,core::AppError> PipeWireCapture::create(PortalStreamConnection connection,std::function<void()> wake) {
  if (connection.fd.get()<0) return std::unexpected(core::AppError{core::AppErrorCode::SourceUnavailable});
  try { return std::unique_ptr<PipeWireCapture>{new PipeWireCapture{std::make_unique<detail::PipeWireWorker>(std::move(connection),std::move(wake))}}; }
  catch (...) { return std::unexpected(core::AppError{core::AppErrorCode::RuntimeConstructionFailure}); }
}
std::expected<void,core::AppError> PipeWireCapture::start() noexcept {
  if (worker_->started || worker_->stopping) return std::unexpected(core::AppError{core::AppErrorCode::InvalidCommand});
  worker_->started=true; worker_->finished=false;
  try { worker_->thread=std::thread{[owner=worker_.get()] { owner->run(); }}; }
  catch (...) { worker_->pool.fail({core::AppErrorCode::RuntimeConstructionFailure}); worker_->wake={}; worker_->finished=true;
    return std::unexpected(core::AppError{core::AppErrorCode::RuntimeConstructionFailure}); }
  return {};
}
PipeWireProgress PipeWireCapture::progress(std::chrono::steady_clock::time_point now) noexcept {
  // Storage reconfiguration stays on the native worker; this method observes
  // bounded facts and never allocates an image while the host is progressing.
  PipeWireProgress p; p.frame=worker_->pool.facts();
  p.quiescent=worker_->finished.load(std::memory_order_acquire);
  p.made_progress=p.frame.epoch!=observed_epoch_ || p.quiescent!=observed_quiescent_;
  observed_epoch_=p.frame.epoch; observed_quiescent_=p.quiescent;
  if (p.quiescent && worker_->thread.joinable()) worker_->thread.join();
  if (!p.quiescent) p.next_wake=now+std::chrono::milliseconds{10};
  return p;
}
std::optional<LinuxCapturedFrame> PipeWireCapture::take_latest() noexcept { return worker_->pool.take_latest(); }
std::expected<void,core::AppError> PipeWireCapture::acknowledge_allocation_revision(std::uint64_t r) noexcept { return worker_->pool.acknowledge(r); }
std::expected<detail::CurrentFirstFrameBinding,core::AppError> PipeWireCapture::bind_current_first_frame(std::uint64_t r) noexcept { return worker_->pool.bind_current_first_frame(r); }
void PipeWireCapture::request_stop() noexcept {
  worker_->pool.request_stop(); worker_->stopping=true;
  std::lock_guard lock{worker_->event_mutex};
  if (worker_->stop_event) (void)pw_loop_signal_event(worker_->loop,worker_->stop_event);
  if (!worker_->started) { worker_->connection.fd=PortalUniqueFd{}; worker_->connection.session={}; worker_->wake={}; }
}
}
