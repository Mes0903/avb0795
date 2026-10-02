/// @file
/// @brief Private GIO dispatch and acknowledged Portal ownership retirement.
#include "platform/linux/portal_screencast.hpp"
#include "platform/linux/detail/acquisition_policy.hpp"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace avb::linux_platform {
namespace {
constexpr auto portal_name = "org.freedesktop.portal.Desktop";
constexpr auto desktop_path = "/org/freedesktop/portal/desktop";
constexpr auto screen_iface = "org.freedesktop.portal.ScreenCast";
constexpr auto request_iface = "org.freedesktop.portal.Request";
constexpr auto session_iface = "org.freedesktop.portal.Session";
constexpr int method_timeout_ms = 5000;
core::AppError error(core::AppErrorCode code, GError *native = nullptr)
{ return {code, native ? std::optional<std::int64_t>{native->code} : std::nullopt}; }
core::AppError native_error(GError *native, bool remote = false)
{
  if (native && g_error_matches(native,G_DBUS_ERROR,G_DBUS_ERROR_ACCESS_DENIED)) return error(core::AppErrorCode::PermissionDenied,native);
  if (native && (g_error_matches(native,G_DBUS_ERROR,G_DBUS_ERROR_SERVICE_UNKNOWN) ||
                 g_error_matches(native,G_DBUS_ERROR,G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
                 g_error_matches(native,G_IO_ERROR,G_IO_ERROR_CLOSED))) return error(core::AppErrorCode::SourceUnavailable,native);
  return error(remote ? core::AppErrorCode::RuntimeConstructionFailure : core::AppErrorCode::PlatformOperationFailure,native);
}
std::expected<std::string,core::AppError> resolve_address(const std::string &explicit_address)
{
  if (!explicit_address.empty()) return explicit_address;
  GError *native{};
  auto *value=g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION,nullptr,&native);
  if (!value) { auto failure=error(core::AppErrorCode::SourceUnavailable,native); g_clear_error(&native); return std::unexpected(failure); }
  std::string address{value}; g_free(value); return address;
}
std::expected<PortalCapabilities,core::AppError> parse_capabilities(GVariant *reply)
{
  if (!reply || !g_variant_is_of_type(reply,G_VARIANT_TYPE("(a{sv})"))) return std::unexpected(error(core::AppErrorCode::SourceUnavailable));
  auto *properties=g_variant_get_child_value(reply,0);
  PortalCapabilities result;
  const bool valid=g_variant_lookup(properties,"version","u",&result.version) &&
                   g_variant_lookup(properties,"AvailableSourceTypes","u",&result.available_source_types) && result.version>=5;
  g_variant_unref(properties);
  if (!valid) return std::unexpected(error(core::AppErrorCode::SourceUnavailable));
  return result;
}
GVariant *empty_options()
{ GVariantBuilder b; g_variant_builder_init(&b,G_VARIANT_TYPE_VARDICT); return g_variant_builder_end(&b); }
}
PortalUniqueFd::~PortalUniqueFd() { if (fd_>=0) ::close(fd_); }
PortalUniqueFd::PortalUniqueFd(PortalUniqueFd &&other) noexcept : fd_{other.release()} {}
PortalUniqueFd &PortalUniqueFd::operator=(PortalUniqueFd &&other) noexcept
{ if(this!=&other) { if(fd_>=0) ::close(fd_); fd_=other.release(); } return *this; }
int PortalUniqueFd::release() noexcept { return std::exchange(fd_,-1); }

std::expected<PortalCapabilities,core::AppError> probe_portal_capabilities(const std::string &bus_address)
{
  auto address=resolve_address(bus_address);
  if(!address) return std::unexpected(address.error());
  GError *native{};
  auto *connection=g_dbus_connection_new_for_address_sync(address->c_str(),GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),nullptr,nullptr,&native);
  if(!connection) { auto failure=error(core::AppErrorCode::SourceUnavailable,native); g_clear_error(&native); return std::unexpected(failure); }
  g_dbus_connection_set_exit_on_close(connection,FALSE);
  auto *reply=g_dbus_connection_call_sync(connection,portal_name,desktop_path,"org.freedesktop.DBus.Properties","GetAll",g_variant_new("(s)",screen_iface),G_VARIANT_TYPE("(a{sv})"),G_DBUS_CALL_FLAGS_NONE,method_timeout_ms,nullptr,&native);
  auto result=reply ? parse_capabilities(reply) : std::expected<PortalCapabilities,core::AppError>{std::unexpected(native_error(native))};
  if(reply) g_variant_unref(reply);
  g_clear_error(&native);
  g_dbus_connection_close_sync(connection,nullptr,nullptr); g_object_unref(connection);
  return result;
}
namespace detail {
/// @brief Owned lifetime facts only; consumer copies never keep native dispatch alive.
/// Native subscriptions and cleanup commands are deliberately stored in the worker,
/// not here. Atomic terminalization permits lease observations on the main thread.
struct PortalSessionState {
  std::atomic<bool> live{true}, started{false}, retired{false};
  std::string path;
};
/// @brief One private context owns all GIO calls and native bookkeeping.
/// Main-thread admission and a bounded facts slot are mutex protected. Commands own
/// copied inputs; GIO callbacks and subscription storage borrow this retained owner
/// until their explicit counters retire. The worker exits only after transport close
/// and every destruction acknowledgment. No external lifetime lease enters that proof.
class PortalWorker final : public std::enable_shared_from_this<PortalWorker> {
  enum class Step { Create, Select, Start, Remote };
  struct Operation;
  /// @brief Method reply and Response rendezvous, retaining obsolete operation identity.
  /// Both subscriptions own this storage until their destroy notifications. Early
  /// responses are bounded and matched only against the returned method handle.
  struct Request {
    std::shared_ptr<Operation> operation;
    std::string predicted, actual;
    guint predicted_subscription{}, alternate_subscription{};
    bool method_done{}, close_predicted{};
    std::map<std::string,GVariant *> early;
    ~Request() { for(auto &[path,value]:early) g_variant_unref(value); }
  };
  /// @brief One acquisition remains owned until calls and request subscriptions retire.
  /// A cancelled policy revokes publication but does not hide side effects. Fresh
  /// failures also wait for session cleanup. Native completion prepares a remote;
  /// publication waits for callback/destruction acknowledgments, and the operation
  /// stays admitted until main-thread transfer or acknowledged revocation. Successful
  /// transferred sessions remain separately retained for remote-only resume.
  struct Operation {
    app::SourceAcquisitionRequest intent;
    AcquisitionPolicy policy;
    std::shared_ptr<PortalSessionState> session;
    std::shared_ptr<Request> request;
    Step step{Step::Create};
    PortalResumeTarget target;
    GCancellable *cancellable{g_cancellable_new()};
    unsigned calls{}, request_borrows{};
    bool fresh{true}, cancelled{}, terminal{}, success{}, published{};
    std::optional<PortalStreamConnection> prepared;
    std::optional<core::AppError> failure;
    ~Operation() { g_object_unref(cancellable); }
  };
  /// @brief Worker-only native session obligations, separate from consumer shared facts.
  /// Confirmed closure, the Close callback and subscription destroy acknowledgment
  /// jointly retire a session. Uncertain Close or owner/transport loss additionally
  /// requires actual GIO transport closure while this record remains owned. The last
  /// published acquisition binds idle failure reporting to this session: retained predecessors may close
  /// during another acquisition without contaminating its result. Consumers never
  /// own this record.
  struct Session {
    std::shared_ptr<PortalSessionState> state;
    std::optional<app::AcquisitionId> last_result_id;
    guint subscription{};
    unsigned subscription_borrows{};
    bool known{}, closing{}, close_done{}, close_pending{}, transport_required{};
  };
  enum class CommandKind { Begin, Consumed, Cancel, Shutdown, Close };
  struct Command {
    CommandKind kind;
    app::SourceAcquisitionRequest intent{};
    std::optional<PortalResumeTarget> resume;
    std::shared_ptr<PortalSessionState> session;
  };
  enum class CallKind { Connect, Properties, Request, Remote, RequestClose, SessionClose, TransportClose };
  /// @brief Async completion storage keeps operation/handle values alive until finish.
  /// The owner borrow is covered by calls_; finish is invoked even for obsolete work.
  struct Call {
    PortalWorker *owner;
    CallKind kind;
    std::shared_ptr<Operation> operation;
    std::shared_ptr<Request> request;
    std::shared_ptr<PortalSessionState> session;
  };
  struct Subscription {
    PortalWorker *owner;
    std::shared_ptr<Request> request;
    std::shared_ptr<PortalSessionState> session;
    bool alternate{};
  };
public:
  PortalWorker(std::string parent,std::function<void()> wake,std::string address)
      : parent_{std::move(parent)},wake_{std::move(wake)},address_{std::move(address)},context_{g_main_context_new()} {}
  ~PortalWorker()
  {
    // An uncertain native owner must use the host's abandonment path. Joining a
    // live thread here would turn ordinary stack unwinding into unbounded blocking.
    if(thread_.joinable()) std::terminate();
    g_main_context_unref(context_);
  }
  std::expected<void,core::AppError> begin(const app::SourceAcquisitionRequest &intent,std::optional<PortalResumeTarget> resume)
  {
    std::lock_guard lock{mutex_};
    if(shutdown_requested_ || active_ || facts_.connection || facts_.close_authority || facts_.error || facts_.cancelled) return std::unexpected(error(core::AppErrorCode::InvalidCommand));
    if(intent.resume_source && !resume) return std::unexpected(error(core::AppErrorCode::SourceUnavailable));
    if(resume && (!resume->session.valid() || !resume->node_id)) return std::unexpected(error(core::AppErrorCode::SourceUnavailable));
    if(!thread_.joinable()) {
      try { thread_=std::thread([this] { run(); }); }
      catch(const std::system_error &) { return std::unexpected(error(core::AppErrorCode::PlatformOperationFailure)); }
    }
    active_=true; cancelled_main_=false; facts_.id=intent.id; facts_.error.reset(); facts_.cancelled=false;
    facts_.phase=resume ? app::AcquisitionPhase::PreparingStream : app::AcquisitionPhase::AwaitingPermission;
    commands_.push_back({CommandKind::Begin,intent,std::move(resume),{}}); notify_command();
    return {};
  }
  void cancel(app::AcquisitionId id) noexcept
  {
    std::lock_guard lock{mutex_};
    if(facts_.id!=id || (!active_ && !facts_.connection)) return;
    cancelled_main_=true; facts_.phase=app::AcquisitionPhase::Cancelling;
    // A result still in the mailbox belongs to the service. Stop must revoke it
    // immediately, even before the worker can process the cancellation command.
    facts_.connection.reset(); facts_.close_authority.reset();
    commands_.push_back({CommandKind::Cancel,{id},{},{}}); notify_command();
  }
  void shutdown() noexcept
  {
    std::lock_guard lock{mutex_};
    if(shutdown_requested_) return;
    shutdown_requested_=true; cancelled_main_=true;
    facts_.connection.reset(); facts_.close_authority.reset();
    if(!thread_.joinable()) { wake_={}; finished_=true; return; }
    commands_.push_back({CommandKind::Shutdown,{},{},{}}); notify_command();
  }
  void close(std::shared_ptr<PortalSessionState> state) noexcept
  {
    std::lock_guard lock{mutex_};
    if(finished_ || shutdown_requested_ || !state->live) return;
    commands_.push_back({CommandKind::Close,{},{},std::move(state)}); notify_command();
  }
  PortalProgress progress(std::chrono::steady_clock::time_point now) noexcept
  {
    PortalProgress result;
    bool join{};
    {
      std::lock_guard lock{mutex_};
      result.id=facts_.id; result.phase=facts_.phase;
      result.error=std::exchange(facts_.error,{}); result.cancelled=std::exchange(facts_.cancelled,false);
      result.connection=std::move(facts_.connection); facts_.connection.reset();
      result.close_authority=std::move(facts_.close_authority); facts_.close_authority.reset();
      if(result.connection) {
        // Publication has already drained the operation's native callbacks. The
        // mutex makes transfer versus cancellation one decision; a queued owning
        // acknowledgment then retires the worker's original operation before any
        // subsequent Begin command. This permits immediate retained resume.
        consumed_id_=result.id;
        active_=false;
        commands_.push_back({CommandKind::Consumed,{result.id},{},{}});
        notify_command();
      }
      result.made_progress=std::exchange(facts_.made_progress,false);
      result.quiescent=finished_ || !thread_.joinable();
      join=finished_ && thread_.joinable();
      if(!result.quiescent && shutdown_requested_) result.next_wake=now+std::chrono::milliseconds{10};
    }
    if(join) thread_.join();
    return result;
  }
private:
  std::string parent_;
  std::function<void()> wake_;
  std::string address_;
  GMainContext *context_;
  GMainLoop *loop_{};
  GSource *command_source_{};
  GDBusConnection *connection_{};
  guint owner_subscription_{};
  gulong connection_closed_handler_{};
  std::thread thread_;
  std::mutex mutex_;
  std::deque<Command> commands_;
  PortalProgress facts_;
  bool active_{}, cancelled_main_{}, shutdown_requested_{}, finished_{};
  std::optional<app::AcquisitionId> consumed_id_;
  bool stopping_{}, transport_failed_{}, transport_closing_{}, transport_closed_{}, connecting_{};
  unsigned calls_{}, subscriptions_{};
  std::optional<PortalCapabilities> capabilities_;
  std::shared_ptr<Operation> operation_;
  std::vector<Session> sessions_;
  void notify_command() noexcept { g_main_context_wakeup(context_); if(command_source_) g_source_set_ready_time(command_source_,0); }
  void acknowledge()
  {
    { std::lock_guard lock{mutex_}; facts_.made_progress=true; }
    if(wake_) wake_();
    schedule_pump();
  }
  void schedule_pump() { g_source_set_ready_time(command_source_,0); }
  static gboolean source_prepare(GSource *,gint *timeout) { *timeout=-1; return FALSE; }
  static gboolean source_check(GSource *) { return FALSE; }
  static gboolean source_dispatch(GSource *,GSourceFunc callback,gpointer data) { return callback(data); }
  static gboolean pump_callback(gpointer data) { static_cast<PortalWorker *>(data)->pump(); return G_SOURCE_CONTINUE; }
  void run()
  {
    g_main_context_push_thread_default(context_);
    loop_=g_main_loop_new(context_,FALSE);
    static GSourceFuncs funcs{source_prepare,source_check,source_dispatch,nullptr,nullptr,nullptr};
    auto *source=g_source_new(&funcs,sizeof(GSource));
    g_source_set_callback(source,pump_callback,this,nullptr); g_source_set_ready_time(source,0); g_source_attach(source,context_);
    { std::lock_guard lock{mutex_}; command_source_=source; }
    g_main_loop_run(loop_);
    { std::lock_guard lock{mutex_}; command_source_=nullptr; }
    g_source_destroy(source); g_source_unref(source);
    g_main_loop_unref(loop_); loop_=nullptr;
    g_main_context_pop_thread_default(context_);
    // Nothing may signal the host after this acknowledgment. The context itself
    // remains an owned, inactive value until the main thread destroys the owner.
    wake_={};
    { std::lock_guard lock{mutex_}; finished_=true; facts_.made_progress=true; }
  }
  Call *call(CallKind kind,std::shared_ptr<Operation> op={},std::shared_ptr<Request> req={},std::shared_ptr<PortalSessionState> state={})
  {
    ++calls_; if(op) ++op->calls;
    return new Call{this,kind,std::move(op),std::move(req),std::move(state)};
  }
  static void callback(GObject *source,GAsyncResult *result,gpointer data)
  {
    std::unique_ptr<Call> invocation{static_cast<Call *>(data)};
    auto &self=*invocation->owner;
    GError *native{}; GVariant *reply{}; GUnixFDList *list{};
    if(invocation->kind==CallKind::Connect) self.connection_=g_dbus_connection_new_for_address_finish(result,&native);
    else if(invocation->kind==CallKind::TransportClose) g_dbus_connection_close_finish(G_DBUS_CONNECTION(source),result,&native);
    else if(invocation->kind==CallKind::Remote) reply=g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source),&list,result,&native);
    else reply=g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),result,&native);
    --self.calls_; if(invocation->operation) --invocation->operation->calls;
    self.completed(*invocation,reply,list,native);
    if(reply) g_variant_unref(reply); if(list) g_object_unref(list); g_clear_error(&native);
    self.acknowledge();
  }
  static void subscription_destroy(gpointer data)
  {
    std::unique_ptr<Subscription> borrow{static_cast<Subscription *>(data)};
    --borrow->owner->subscriptions_;
    if(borrow->request) --borrow->request->operation->request_borrows;
    if(borrow->session) if(auto *session=borrow->owner->find_session(borrow->session)) --session->subscription_borrows;
    borrow->owner->acknowledge();
  }
  guint subscribe(const char *sender,const char *iface,const char *signal,const char *path,const char *arg,Subscription *storage)
  {
    ++subscriptions_; if(storage->request) ++storage->request->operation->request_borrows;
    if(storage->session) if(auto *session=find_session(storage->session)) ++session->subscription_borrows;
    return g_dbus_connection_signal_subscribe(connection_,sender,iface,signal,path,arg,G_DBUS_SIGNAL_FLAGS_NONE,signal_callback,storage,subscription_destroy);
  }
  void unsubscribe(guint &id) { if(id) g_dbus_connection_signal_unsubscribe(connection_,std::exchange(id,0)); }
  static void signal_callback(GDBusConnection *,const gchar *,const gchar *path,const gchar *iface,const gchar *,GVariant *value,gpointer data)
  {
    auto &storage=*static_cast<Subscription *>(data); auto &self=*storage.owner;
    if(std::strcmp(iface,request_iface)==0) {
      auto req=storage.request;
      if(storage.alternate && req->predicted==path) return;
      // The exact predicted subscription is established before the method call.
      // A short-lived broad subscription also retains an early alternate handle;
      // only the subsequently returned path can authorize that response.
      const auto prefix=self.path_prefix("request");
      if(!std::string_view{path}.starts_with(prefix)) return;
      if(req->method_done && req->actual!=path) return;
      if(!req->early.contains(path)) {
        if(req->early.size()>=4) { self.fail(req->operation,error(core::AppErrorCode::PlatformOperationFailure)); return; }
        req->early.emplace(path,g_variant_ref(value));
      }
      if(req->method_done) self.reconcile(req);
    } else if(std::strcmp(iface,session_iface)==0) {
      storage.session->live=false; storage.session->started=false;
      if(auto *session=self.find_session(storage.session)) {
        session->known=true; session->close_done=true; self.unsubscribe(session->subscription);
        if(!session->closing && self.operation_ && self.operation_->session==storage.session && !self.transferred(self.operation_))
          self.fail(self.operation_,error(core::AppErrorCode::SourceUnavailable));
        {
          std::lock_guard lock{self.mutex_};
          // An undelivered remote is still service-owned. Closed must revoke it
          // before main-thread consumption, just as explicit cancellation does.
          if(self.facts_.connection && self.facts_.connection->session.state_==storage.session) {
            self.facts_.connection.reset();
            self.facts_.close_authority.reset();
          }
          if(!session->closing && !self.active_ && session->last_result_id==self.facts_.id)
            self.facts_.error=error(core::AppErrorCode::SourceUnavailable);
        }
      }
    } else {
      const char *name{}, *old_owner{}, *new_owner{};
      if(g_variant_is_of_type(value,G_VARIANT_TYPE("(sss)"))) {
        g_variant_get(value,"(&s&s&s)",&name,&old_owner,&new_owner);
        if(*old_owner && std::strcmp(old_owner,new_owner)!=0) self.transport_lost();
      }
    }
    self.acknowledge();
  }
  static void transport_signal(GDBusConnection *,gboolean,GError *,gpointer data)
  { auto &self=*static_cast<PortalWorker *>(data); self.transport_lost(); self.acknowledge(); }
  void transport_lost()
  {
    if(transport_closing_) return;
    stopping_ = true;
    transport_failed_ = true;
    {
      std::lock_guard lock{mutex_};
      shutdown_requested_=true; cancelled_main_=true;
      // The remote and Close authority are still ours until progress transfers
      // them. Transport loss must revoke both under the publication lock.
      facts_.connection.reset(); facts_.close_authority.reset();
      if(!operation_ || consumed_id_==operation_->intent.id) {
        // A queued new Begin owns the current identity and will fail on the
        // worker below. Otherwise this is loss of an already transferred source.
        if(!active_) { facts_.error=error(core::AppErrorCode::SourceUnavailable); facts_.phase=app::AcquisitionPhase::Idle; }
      }
    }
    // Loss revokes use, but cannot manufacture confirmed Session closure. Retain
    // each native record until the actual transport and its own borrows retire.
    for (auto &session : sessions_) {
      session.state->live = false;
      session.state->started = false;
      session.transport_required = true;
      unsubscribe(session.subscription);
    }
    if(operation_ && !transferred(operation_)) fail(operation_,error(core::AppErrorCode::SourceUnavailable));
    unsubscribe(owner_subscription_);
  }
  Session *find_session(const std::shared_ptr<PortalSessionState> &state)
  { auto it=std::find_if(sessions_.begin(),sessions_.end(),[&](auto &entry){return entry.state==state;}); return it==sessions_.end()?nullptr:&*it; }
  std::string path_prefix(const char *kind)
  {
    std::string sender{g_dbus_connection_get_unique_name(connection_)+1};
    std::replace(sender.begin(),sender.end(),'.','_');
    return std::string{desktop_path}+"/"+kind+"/"+sender+"/";
  }
  static std::string token()
  { auto *uuid=g_uuid_string_random(); std::string value{"avb_"}; value+=uuid; g_free(uuid); std::replace(value.begin(),value.end(),'-','_'); return value; }
  bool acceptable(const std::shared_ptr<Operation> &op)
  {
    std::lock_guard lock{mutex_};
    return !shutdown_requested_ && !cancelled_main_ && facts_.id==op->intent.id && op->policy.accepts(op->intent.id,op->session && op->session->live);
  }
  bool transferred(const std::shared_ptr<Operation> &op)
  {
    std::lock_guard lock{mutex_};
    return consumed_id_==op->intent.id;
  }
  void pump()
  {
    g_source_set_ready_time(command_source_,-1);
    std::deque<Command> work;
    { std::lock_guard lock{mutex_}; work.swap(commands_); }
    for(auto &command:work) {
      if(command.kind==CommandKind::Begin) {
        operation_=std::make_shared<Operation>(); operation_->intent=command.intent; operation_->policy.begin(command.intent.id);
        if(command.resume) {
          operation_->fresh=false; operation_->target=*command.resume; operation_->session=command.resume->session.state_; operation_->step=Step::Remote;
          auto *session=find_session(operation_->session);
          if(!session || session->closing || !operation_->session->started) { fail(operation_,error(core::AppErrorCode::SourceUnavailable)); continue; }
        }
        if(stopping_) fail(operation_,error(core::AppErrorCode::SourceUnavailable));
        else if(connection_) start_operation(); else connect();
      } else if(command.kind==CommandKind::Consumed) {
        if(operation_ && operation_->intent.id==command.intent.id && transferred(operation_)) {
          operation_->policy.retire(); operation_.reset();
          std::lock_guard lock{mutex_}; consumed_id_.reset();
        }
      } else if(command.kind==CommandKind::Cancel) {
        if(operation_ && operation_->intent.id==command.intent.id) cancel_operation(operation_);
      } else if(command.kind==CommandKind::Close) close_session(command.session);
      else {
        stopping_=true;
        if(operation_) { operation_->policy.shutdown(); cancel_operation(operation_); }
        for(auto &session:sessions_) close_session(session.state);
        unsubscribe(owner_subscription_);
      }
      acknowledge();
    }
    // Uncertain session cleanup needs transport proof. Initiate it while native
    // records/calls are retained; waiting for their retirement first is circular.
    if (transport_failed_) close_transport();
    std::erase_if(sessions_, [this](const auto &session) {
      if (session.transport_required && !transport_closed_) return false;
      if ((!session.close_done && !session.transport_required) || session.close_pending || session.subscription_borrows) return false;
      // Pump runs after callback/destroy scopes return. Invalidity only revokes use;
      // this monotonic fact acknowledges the session's completed native obligations.
      session.state->retired = true;
      acknowledge();
      return true;
    });
    retire_operation();
    if(!stopping_ || operation_ || calls_ || subscriptions_ || !sessions_.empty()) return;
    { std::lock_guard lock{mutex_}; if(!commands_.empty()) return; }
    if (connection_ && !transport_closed_) {
      close_transport();
      return;
    }
    if(connection_) { g_object_unref(connection_); connection_=nullptr; }
    g_main_loop_quit(loop_);
  }
  /// @brief Start asynchronous transport cleanup without releasing borrowed records.
  /// Loss calls this before session/operation retirement; ordinary shutdown calls it
  /// after their independent cleanup. Completion proves actual GIO closure, while
  /// pending method callbacks and subscription destroys remain separately counted.
  /// Only the private worker context calls this; submission is one-shot, never proof.
  void close_transport()
  {
    if (!connection_ || transport_closing_ || transport_closed_) return;
    transport_closing_ = true;
    if (connection_closed_handler_) g_signal_handler_disconnect(connection_, std::exchange(connection_closed_handler_, 0));
    g_dbus_connection_close(connection_, nullptr, callback, call(CallKind::TransportClose));
  }
  void connect()
  {
    if(connecting_) return;
    auto address=resolve_address(address_);
    if(!address) { fail(operation_,address.error()); return; }
    connecting_=true;
    g_dbus_connection_new_for_address(address->c_str(),GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),nullptr,operation_->cancellable,callback,call(CallKind::Connect,operation_));
  }
  /// @brief Advance only after successful capability discovery on the private connection.
  /// A live bus connection does not prove Portal availability or valid properties.
  /// Failed discovery leaves no capability facts, so a later admitted acquisition
  /// retries GetAll through the same counted native completion path. Cancellation
  /// and shutdown still retire the original operation before another can be admitted.
  void start_operation()
  {
    auto op=operation_; if(!op || op->terminal) return;
    if(op->cancelled || stopping_) { op->terminal=true; return; }
    if(!capabilities_) {
      g_dbus_connection_call(connection_,portal_name,desktop_path,"org.freedesktop.DBus.Properties","GetAll",g_variant_new("(s)",screen_iface),G_VARIANT_TYPE("(a{sv})"),G_DBUS_CALL_FLAGS_NONE,method_timeout_ms,op->cancellable,callback,call(CallKind::Properties,op));
      return;
    }
    if(!op->fresh) { remote(op); return; }
    const auto types=op->intent.kind==core::SourceKind::Display?1u:2u;
    if(!(capabilities_->available_source_types&types)) { fail(op,error(core::AppErrorCode::SourceUnavailable)); return; }
    op->session=std::make_shared<PortalSessionState>();
    op->session->path=path_prefix("session")+token();
    sessions_.push_back(Session{op->session});
    sessions_.back().subscription=subscribe(portal_name,session_iface,"Closed",op->session->path.c_str(),nullptr,new Subscription{this,{},op->session});
    request(op,Step::Create);
  }
  void request(const std::shared_ptr<Operation> &op,Step step)
  {
    op->step=step;
    auto req=std::make_shared<Request>(); req->operation=op;
    const auto handle_token=token(); req->predicted=path_prefix("request")+handle_token;
    req->predicted_subscription=subscribe(portal_name,request_iface,"Response",req->predicted.c_str(),nullptr,new Subscription{this,req,{}});
    req->alternate_subscription=subscribe(portal_name,request_iface,"Response",nullptr,nullptr,new Subscription{this,req,{},true});
    op->request=req;
    GVariantBuilder options; g_variant_builder_init(&options,G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options,"{sv}","handle_token",g_variant_new_string(handle_token.c_str()));
    const char *method{}; GVariant *args{};
    if(step==Step::Create) {
      method="CreateSession"; const auto last=op->session->path.find_last_of('/');
      g_variant_builder_add(&options,"{sv}","session_handle_token",g_variant_new_string(op->session->path.substr(last+1).c_str()));
      args=g_variant_new("(a{sv})",&options);
    } else if(step==Step::Select) {
      method="SelectSources";
      g_variant_builder_add(&options,"{sv}","multiple",g_variant_new_boolean(FALSE));
      g_variant_builder_add(&options,"{sv}","types",g_variant_new_uint32(op->intent.kind==core::SourceKind::Display?1:2));
      args=g_variant_new("(oa{sv})",op->session->path.c_str(),&options);
    } else { method="Start"; args=g_variant_new("(osa{sv})",op->session->path.c_str(),parent_.c_str(),&options); }
    g_dbus_connection_call(connection_,portal_name,desktop_path,screen_iface,method,args,G_VARIANT_TYPE("(o)"),G_DBUS_CALL_FLAGS_NONE,method_timeout_ms,op->cancellable,callback,call(CallKind::Request,op,req));
  }
  void close_request(const std::shared_ptr<Request> &req,const std::string &path)
  {
    // Request.Close emits no Response. It has a separate uncancelled call and its
    // completion, including UnknownObject, is retained. The method reply triggers
    // another Close of the actual handle to reconcile close-before-creation races.
    g_dbus_connection_call(connection_,portal_name,path.c_str(),request_iface,"Close",nullptr,G_VARIANT_TYPE_UNIT,G_DBUS_CALL_FLAGS_NO_AUTO_START,method_timeout_ms,nullptr,callback,call(CallKind::RequestClose,req->operation,req));
  }
  void detach_request(const std::shared_ptr<Request> &req)
  {
    unsubscribe(req->predicted_subscription); unsubscribe(req->alternate_subscription);
    if(req->operation->request==req) req->operation->request.reset();
  }
  void close_session(const std::shared_ptr<PortalSessionState> &state)
  {
    auto *session=find_session(state); if(!session) return;
    session->closing=true; state->live=false; state->started=false;
    if (!session->known || session->close_done || session->close_pending || session->transport_required) return;
    session->close_pending=true;
    g_dbus_connection_call(connection_,portal_name,state->path.c_str(),session_iface,"Close",nullptr,G_VARIANT_TYPE_UNIT,G_DBUS_CALL_FLAGS_NO_AUTO_START,method_timeout_ms,nullptr,callback,call(CallKind::SessionClose,{}, {},state));
  }
  void cancel_operation(const std::shared_ptr<Operation> &op)
  {
    op->cancelled=true; op->success=false; op->prepared.reset(); op->policy.cancel(op->intent.id);
    {
      std::lock_guard lock{mutex_};
      if(facts_.id==op->intent.id) { facts_.connection.reset(); facts_.close_authority.reset(); }
    }
    if(op->request) {
      auto req=op->request;
      if(!req->close_predicted) { req->close_predicted=true; close_request(req,req->predicted); }
      if(req->method_done) { if(req->actual!=req->predicted) close_request(req,req->actual); detach_request(req); op->terminal=true; }
    } else if(op->step!=Step::Remote || !op->calls) op->terminal=true;
    if(op->fresh && op->session) close_session(op->session);
    // We deliberately keep side-effecting calls uncancelled. Cancelling GIO is
    // not rollback and would hide the late handle or FD required for cleanup.
    // Initial connection setup has no Portal side effects and may be cancelled.
    if(connecting_) g_cancellable_cancel(op->cancellable);
  }
  /// @brief Retain a native failure unless cancellation already revoked this operation.
  /// Late erroneous replies still need the same handle/session cleanup, but cannot
  /// replace explicit cancellation with Failed. Re-entering cleanup also reconciles
  /// a method completion that made a previously pending request closeable.
  void fail(const std::shared_ptr<Operation> &op,core::AppError failure)
  {
    if(!op || transferred(op)) return;
    if(op->cancelled) { cancel_operation(op); return; }
    if(!op->failure) op->failure=failure;
    cancel_operation(op); op->cancelled=false;
    if(!op->request && !op->calls) op->terminal=true;
  }
  void reconcile(const std::shared_ptr<Request> &req)
  {
    auto op=req->operation;
    auto found=req->early.find(req->actual);
    if(found==req->early.end()) {
      if(op->cancelled || op->failure || stopping_) { detach_request(req); op->terminal=true; if(op->fresh) close_session(op->session); }
      return;
    }
    auto *response=found->second;
    if(!g_variant_is_of_type(response,G_VARIANT_TYPE("(ua{sv})"))) { detach_request(req); fail(op,error(core::AppErrorCode::PlatformOperationFailure)); return; }
    guint code{}; GVariant *results{}; g_variant_get(response,"(u@a{sv})",&code,&results);
    const auto disposition=response_disposition(code);
    if(disposition!=ResponseDisposition::Success) {
      g_variant_unref(results); detach_request(req);
      if(disposition==ResponseDisposition::Cancelled) { cancel_operation(op); op->terminal=true; }
      else fail(op,error(core::AppErrorCode::PlatformOperationFailure));
      return;
    }
    if(op->step==Step::Create) {
      const char *path{};
      if(!g_variant_lookup(results,"session_handle","&s",&path) || !g_variant_is_object_path(path)) {
        g_variant_unref(results); detach_request(req); fail(op,error(core::AppErrorCode::RuntimeConstructionFailure)); return;
      }
      if(op->session->path!=path) {
        auto *session=find_session(op->session); unsubscribe(session->subscription); op->session->path=path;
        session->subscription=subscribe(portal_name,session_iface,"Closed",path,nullptr,new Subscription{this,{},op->session});
      }
      if(auto *session=find_session(op->session)) session->known=true;
    } else if(op->step==Step::Start) {
      auto *streams=g_variant_lookup_value(results,"streams",G_VARIANT_TYPE("a(ua{sv})"));
      if(!streams || g_variant_n_children(streams)!=1) {
        if(streams) g_variant_unref(streams); g_variant_unref(results); detach_request(req); fail(op,error(core::AppErrorCode::RuntimeConstructionFailure)); return;
      }
      auto *stream=g_variant_get_child_value(streams,0); GVariant *metadata{}; guint node{};
      g_variant_get(stream,"(u@a{sv})",&node,&metadata); guint64 serial{};
      const bool has_serial=g_variant_lookup(metadata,"pipewire-serial","t",&serial);
      op->target={PortalSessionLease{op->session},node,stream_serial(capabilities_->version,has_serial?std::optional<std::uint64_t>{serial}:std::nullopt)};
      g_variant_unref(metadata); g_variant_unref(stream); g_variant_unref(streams);
      if(!node) { g_variant_unref(results); detach_request(req); fail(op,error(core::AppErrorCode::RuntimeConstructionFailure)); return; }
      if(op->session->live) op->session->started=true;
    }
    g_variant_unref(results); detach_request(req);
    if(!acceptable(op)) {
      if(!op->failure && !op->cancelled && !stopping_ && !op->session->live) op->failure=error(core::AppErrorCode::SourceUnavailable);
      op->terminal=true; if(op->fresh) close_session(op->session); return;
    }
    if(op->step==Step::Create) request(op,Step::Select);
    else if(op->step==Step::Select) request(op,Step::Start);
    else remote(op);
  }
  void remote(const std::shared_ptr<Operation> &op)
  {
    op->step=Step::Remote;
    { std::lock_guard lock{mutex_}; facts_.phase=app::AcquisitionPhase::PreparingStream; }
    g_dbus_connection_call_with_unix_fd_list(connection_,portal_name,desktop_path,screen_iface,"OpenPipeWireRemote",g_variant_new("(o@a{sv})",op->session->path.c_str(),empty_options()),G_VARIANT_TYPE("(h)"),G_DBUS_CALL_FLAGS_NONE,method_timeout_ms,nullptr,op->cancellable,callback,call(CallKind::Remote,op));
  }
  void completed(Call &invocation,GVariant *reply,GUnixFDList *list,GError *native)
  {
    auto op=invocation.operation;
    switch(invocation.kind) {
    case CallKind::Connect:
      connecting_=false;
      if(!connection_) { if(op->cancelled || stopping_) op->terminal=true; else fail(op,error(core::AppErrorCode::SourceUnavailable,native)); break; }
      g_dbus_connection_set_exit_on_close(connection_,FALSE);
      connection_closed_handler_=g_signal_connect(connection_,"closed",G_CALLBACK(transport_signal),this);
      if(stopping_ || op->cancelled) { op->terminal=true; break; }
      owner_subscription_=subscribe("org.freedesktop.DBus","org.freedesktop.DBus","NameOwnerChanged","/org/freedesktop/DBus",portal_name,new Subscription{this});
      start_operation();
      break;
    case CallKind::Properties: {
      if(!reply) { fail(op,native_error(native)); break; }
      auto properties=parse_capabilities(reply);
      if(!properties) { fail(op,properties.error()); break; }
      capabilities_=*properties; start_operation(); break;
    }
    case CallKind::Request: {
      auto req=invocation.request; req->method_done=true;
      if(op->step==Step::Create) if(auto *session=find_session(op->session)) session->known=true;
      if(!reply) {
        close_request(req,req->predicted); detach_request(req); fail(op,native_error(native));
        // A method timeout cannot prove rollback. Start transport close with native
        // records retained, then drain callbacks so late side effects stay accounted.
        if(native && (g_error_matches(native,G_IO_ERROR,G_IO_ERROR_TIMED_OUT) || g_error_matches(native,G_DBUS_ERROR,G_DBUS_ERROR_NO_REPLY))) transport_lost();
        break;
      }
      const char *handle{}; g_variant_get(reply,"(&o)",&handle); req->actual=handle;
      if(!g_variant_is_object_path(handle)) { detach_request(req); fail(op,error(core::AppErrorCode::PlatformOperationFailure)); break; }
      if(req->actual!=req->predicted) {
        unsubscribe(req->predicted_subscription);
        req->predicted_subscription=subscribe(portal_name,request_iface,"Response",req->actual.c_str(),nullptr,new Subscription{this,req,{}});
        unsubscribe(req->alternate_subscription);
      }
      if(op->cancelled || op->failure || stopping_) close_request(req,req->actual);
      reconcile(req); break;
    }
    case CallKind::Remote: {
      PortalUniqueFd descriptor;
      if(reply && list) {
        gint index{-1}; g_variant_get(reply,"(h)",&index);
        if(index>=0 && index<g_unix_fd_list_get_length(list)) {
          GError *fd_error{}; descriptor=PortalUniqueFd{g_unix_fd_list_get(list,index,&fd_error)}; g_clear_error(&fd_error);
        }
      }
      // The handle is a list index. get duplicates one owned CLOEXEC descriptor;
      // unref of the list retires its originals even for malformed or late replies.
      if(descriptor.get()<0) { fail(op,native ? native_error(native,true) : error(core::AppErrorCode::RuntimeConstructionFailure)); break; }
      if(acceptable(op)) {
        std::lock_guard lock{mutex_};
        // Recheck main-thread cancellation under the publication lock.
        if(!shutdown_requested_ && !cancelled_main_ && op->session->live) {
          op->prepared.emplace(PortalStreamConnection{std::move(descriptor),op->target.node_id,op->target.serial,PortalSessionLease{op->session}});
          op->success=true;
        }
      }
      op->terminal=true; if(!op->success && op->fresh) close_session(op->session);
      break;
    }
    case CallKind::RequestClose: break;
    case CallKind::SessionClose:
      if(auto *session=find_session(invocation.session)) {
        session->close_pending=false;
        if (reply || (native && g_error_matches(native, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT))) {
          session->close_done=true; session->state->live=false; session->state->started=false; unsubscribe(session->subscription);
        }
        else {
          // Failure to acknowledge Close cannot be reinterpreted as retirement;
          // retiring the client connection supplies the remaining authority.
          transport_lost();
        }
      }
      break;
    case CallKind::TransportClose:
      // A failed close is accepted only if GIO itself already proves it closed.
      if(!native || g_dbus_connection_is_closed(connection_)) transport_closed_=true;
      break;
    }
  }
  void retire_operation()
  {
    auto op=operation_;
    if(!op || !op->terminal || op->calls || op->request_borrows || op->request) return;
    if(op->fresh && !op->success && op->session) {
      if (!op->session->retired) return;
    }
    if(op->success) {
      bool revoked{}, lost{}, published{};
      {
        std::lock_guard lock{mutex_};
        if(consumed_id_==op->intent.id) return; // owning transfer acknowledgment is queued
        lost=!op->session->live;
        revoked=shutdown_requested_ || cancelled_main_ || lost;
        if(!revoked && !op->published) {
          facts_.connection=std::move(op->prepared); op->prepared.reset();
          if(op->fresh) facts_.close_authority=PortalSessionCloseAuthority{weak_from_this(),op->session};
          if(auto *session=find_session(op->session)) session->last_result_id=op->intent.id;
          facts_.phase=app::AcquisitionPhase::PreparingStream;
          op->published=true; published=true;
        }
      }
      if(revoked) {
        if(lost && !op->cancelled) fail(op,error(core::AppErrorCode::SourceUnavailable));
        else cancel_operation(op);
        schedule_pump();
      } else if(published) acknowledge();
      // A service-owned result keeps its original operation and admission until
      // transfer or revocation. Success-shaped mailbox storage is not retirement.
      return;
    }
    op->policy.retire();
    { std::lock_guard lock{mutex_};
      // Main cancellation may race a callback before its queued command runs.
      // This publication lock makes its already accepted intent authoritative;
      // native callbacks and cleanup have fully retired before the acknowledgment.
      if(facts_.phase==app::AcquisitionPhase::Cancelling) op->failure.reset();
      active_=false; facts_.phase=op->success ? app::AcquisitionPhase::PreparingStream : app::AcquisitionPhase::Idle;
      if(op->failure) facts_.error=op->failure;
      else if(!op->success) facts_.cancelled=true;
    }
    operation_.reset(); acknowledge();
  }
};
}
bool PortalSessionLease::valid() const noexcept { return state_ && state_->live && state_->started; }
bool PortalSessionLease::retired() const noexcept { return !state_ || state_->retired; }
PortalSessionCloseAuthority::PortalSessionCloseAuthority(std::weak_ptr<detail::PortalWorker> worker,std::shared_ptr<detail::PortalSessionState> session)
    : worker_{std::move(worker)},session_{std::move(session)} {}
void PortalSessionCloseAuthority::request_close() noexcept
{ if(session_ && session_->live) if(auto worker=worker_.lock()) worker->close(session_); }
PortalScreencast::PortalScreencast(std::string parent,std::function<void()> wake,std::string address)
    : worker_{std::make_shared<detail::PortalWorker>(std::move(parent),std::move(wake),std::move(address))} {}
PortalScreencast::~PortalScreencast() = default;
std::expected<void,core::AppError> PortalScreencast::begin(const app::SourceAcquisitionRequest &intent,std::optional<PortalResumeTarget> resume)
{ return worker_->begin(intent,std::move(resume)); }
void PortalScreencast::cancel(app::AcquisitionId id) noexcept { worker_->cancel(id); }
void PortalScreencast::request_shutdown() noexcept { worker_->shutdown(); }
PortalProgress PortalScreencast::progress(std::chrono::steady_clock::time_point now) noexcept { return worker_->progress(now); }
}
