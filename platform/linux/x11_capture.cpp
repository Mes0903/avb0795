/// @file
/// @brief Private XCB intake, exact request identity and acknowledged worker retirement.
#include "platform/linux/x11_capture.hpp"
#include "platform/linux/detail/x11_image_policy.hpp"
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/composite.h>
#include <xcb/randr.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace avb::linux_platform {
  namespace detail {
    bool x11_request_current(const X11RequestStamp &stamp, std::uint64_t generation,
                             std::uint64_t resource_epoch, const CpuPoolFacts &facts, bool valid) noexcept
    {
      return valid && generation && resource_epoch && stamp.observation_generation == generation &&
             stamp.resource_epoch == resource_epoch && stamp.allocation_revision && facts.format &&
             stamp.allocation_revision == facts.revision && stamp.allocation_revision == facts.storage_ready_revision &&
             !facts.stopping && !facts.lost;
    }
    bool x11_first_frame_expired(std::optional<std::chrono::steady_clock::time_point> activation,
                                std::chrono::steady_clock::time_point now, const CpuPoolFacts &facts) noexcept
    {
      return activation && !facts.sequence && !facts.stopping && !facts.lost &&
             now >= *activation && now - *activation >= std::chrono::seconds{10};
    }
  }
  namespace {
    using Clock = std::chrono::steady_clock;
    constexpr core::AppError unavailable{core::AppErrorCode::SourceUnavailable};
    constexpr core::AppError unsupported{core::AppErrorCode::UnsupportedGraphics};
    constexpr core::AppError construction_failed{core::AppErrorCode::RuntimeConstructionFailure};
    constexpr core::AppError operation_failed{core::AppErrorCode::PlatformOperationFailure};
    constexpr core::AppError state_lost{core::AppErrorCode::RuntimeStateLost};
    struct Free { void operator()(void *p) const noexcept { std::free(p); } };
    template<class T> using Reply = std::unique_ptr<T, Free>;
    struct Disconnect { void operator()(xcb_connection_t *c) const noexcept { if (c) xcb_disconnect(c); } };
    using Connection = std::unique_ptr<xcb_connection_t, Disconnect>;

    /// @brief Validate protocol coordinates independently of general image arithmetic.
    /// GetImage uses signed origins and unsigned dimensions even when the scalar
    /// image policy can describe larger extents. Subtraction avoids wrapped bounds.
    bool native_region(core::PixelRegion r, core::Extent drawable) noexcept
    {
      return r.x <= INT16_MAX && r.y <= INT16_MAX && r.width && r.height &&
             r.width <= UINT16_MAX && r.height <= UINT16_MAX && r.x <= drawable.width && r.y <= drawable.height &&
             r.width <= drawable.width - r.x && r.height <= drawable.height - r.y;
    }
  }

  /// @brief Transfer this complete inactive state once to the intake worker.
  /// Only the worker touches native state after start. Main owns the thread handle
  /// and activation timestamp; the pool and finished atomic carry observations.
  /// The separate FD mutex serializes nonblocking Stop writes against close, so a
  /// delayed Stop cannot write to an unrelated recycled descriptor. No facts lock
  /// is held across native I/O, image allocation, conversion or the host callback.
  /// A short publication gate serializes consuming commit with the main thread's
  /// deadline reread: a stale sequence-zero snapshot must not fail a first frame
  /// that already published. Its lock order is publication gate then pool facts;
  /// Stop uses only the pool and cannot wait for image work or a native reply.
  struct X11Capture::Impl {
    X11SourceTarget target;
    std::function<void()> wake;
    Connection connection;
    detail::CpuFramePool pool;
    detail::CpuFormat initial{};
    std::uint64_t initial_rev{}, resource_epoch{1}, seen_epoch{};
    std::optional<Clock::time_point> activation;
    std::thread worker;
    std::atomic_bool finished{};
    bool started{}, seen_finished{};
    // Serialize only publication against main-side first-frame expiry. Neither
    // contender holds this mutex during native work, image work or callbacks.
    std::mutex publication_mutex;
    std::mutex signal_mutex;
    int signal_fd{-1};
    std::uint8_t randr_event{};
    xcb_pixmap_t pixmap{};
    bool redirected{}, dirty{true};
    unsigned pending{};
    detail::X11ImageFacts image;

    Impl(X11SourceTarget source, std::function<void()> notify)
        : target(std::move(source)), wake(std::move(notify)) {}
    ~Impl() { retire(); }

    void signal() noexcept
    {
      std::lock_guard lock{signal_mutex};
      if (signal_fd >= 0) {
        const std::uint64_t one = 1;
        // EAGAIN means an earlier wake is already pending. EINTR is retried while
        // the same descriptor is protected from close/reuse.
        while (::write(signal_fd, &one, sizeof(one)) < 0 && errno == EINTR) {}
      }
    }
    /// @brief Release connection-local ownership before publishing quiescence.
    /// Closing the private connection discards outstanding replies and releases
    /// its pixmap names/automatic redirect at the server's client teardown. There
    /// is no final server round trip to strand Stop. External CPU leases survive.
    void retire() noexcept
    {
      if (connection && pending) xcb_discard_reply(connection.get(), pending);
      pending = 0;
      connection.reset();
      pixmap = 0;
      redirected = false;
      {
        std::lock_guard lock{signal_mutex};
        if (signal_fd >= 0) ::close(std::exchange(signal_fd, -1));
      }
      wake = {};
    }
    void fail(core::AppError error) noexcept { pool.fail(error); }
    void protocol_error(const xcb_generic_error_t *error) noexcept
    {
      if (xcb_connection_has_error(connection.get())) fail(unavailable);
      else if (error && error->error_code == XCB_WINDOW) fail({core::AppErrorCode::SourceUnavailable, error->error_code});
      else fail({core::AppErrorCode::PlatformOperationFailure, error ? error->error_code : 0});
    }
    bool active() noexcept
    {
      auto facts = pool.facts();
      if (detail::x11_first_frame_expired(activation, Clock::now(), facts)) {
        fail(state_lost);
        return false;
      }
      return !facts.stopping && !facts.lost;
    }
    /// @brief Consume only genuine connection events before accepting any reply.
    /// Polling a reply can itself queue events, so callers also drain afterwards.
    /// Configure invalidates resource identity immediately, even for equal-sized
    /// storage. A real unmap retires this capture instead of reviving stale pixels.
    void events() noexcept
    {
      while (Reply<xcb_generic_event_t> event{xcb_poll_for_event(connection.get())}) {
        if (event->response_type & 0x80) continue;
        const auto type = event->response_type;
        if (!type) { protocol_error(reinterpret_cast<xcb_generic_error_t *>(event.get())); continue; }
        if ((type == XCB_DESTROY_NOTIFY && reinterpret_cast<xcb_destroy_notify_event_t *>(event.get())->window == target.drawable()) ||
            (type == XCB_UNMAP_NOTIFY && reinterpret_cast<xcb_unmap_notify_event_t *>(event.get())->window == target.drawable())) fail(unavailable);
        if (type == XCB_CONFIGURE_NOTIFY && reinterpret_cast<xcb_configure_notify_event_t *>(event.get())->window == target.drawable()) {
          if (target.kind() == core::SourceKind::Display) fail(unavailable);
          else {
            dirty = true;
            if (resource_epoch == UINT64_MAX) fail(state_lost);
            else ++resource_epoch;
          }
        }
        if (target.kind() == core::SourceKind::Display && randr_event &&
            (type == randr_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY || type == randr_event + XCB_RANDR_NOTIFY)) fail(unavailable);
      }
      if (!target.valid() || xcb_connection_has_error(connection.get())) fail(unavailable);
    }
    void wait_socket(int timeout) noexcept
    {
      pollfd fds[2]{{xcb_get_file_descriptor(connection.get()), POLLIN, 0}, {signal_fd, POLLIN, 0}};
      const auto result = ::poll(fds, 2, timeout);
      if (result < 0 && errno != EINTR) fail({core::AppErrorCode::PlatformOperationFailure, errno});
      if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) fail(unavailable);
      if (fds[1].revents & POLLIN) {
        std::uint64_t value{};
        while (::read(signal_fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
      }
    }
    /// @brief Retain exactly one reply obligation while Stop-aware polling proceeds.
    /// This concrete worker helper also owns setup replies; no live main call waits
    /// for the server. A stalled write/flush still retains the worker owner, and
    /// main-side deadline/Stop closes pool admission independently.
    template<class T> Reply<T> receive(unsigned sequence) noexcept
    {
      pending = sequence;
      xcb_flush(connection.get());
      for (;;) {
        events();
        if (!active()) break;
        void *raw{};
        xcb_generic_error_t *raw_error{};
        const auto ready = xcb_poll_for_reply(connection.get(), sequence, &raw, &raw_error);
        Reply<T> reply{static_cast<T *>(raw)};
        Reply<xcb_generic_error_t> error{raw_error};
        events();
        if (ready) {
          pending = 0;
          if (error || !reply) protocol_error(error.get());
          if (!active()) return {};
          return reply;
        }
        if (!active()) break;
        wait_socket(16);
      }
      xcb_discard_reply(connection.get(), sequence);
      pending = 0;
      return {};
    }
    bool checked(xcb_void_cookie_t cookie) noexcept
    {
      // A later reply proves server processing before request_check, avoiding its
      // otherwise hidden blocking synchronization during live capture.
      const auto fence = receive<xcb_get_input_focus_reply_t>(xcb_get_input_focus(connection.get()).sequence);
      if (!fence) return false;
      Reply<xcb_generic_error_t> error{xcb_request_check(connection.get(), cookie)};
      if (error) { protocol_error(error.get()); return false; }
      return active();
    }
    /// @brief Derive exact packed storage from this screen's checked visual/depth.
    /// GetImage on a pixmap has no visual; window/root attributes remain the visual
    /// authority. Masks outside depth planes and unsupported storage fail closed.
    bool visual(xcb_visualid_t id, std::uint8_t depth) noexcept
    {
      const auto *setup = xcb_get_setup(connection.get());
      auto screen = xcb_setup_roots_iterator(setup);
      for (int i = 0; i < target.screen_number() && screen.rem; ++i) xcb_screen_next(&screen);
      if (!screen.rem || screen.data->root != target.root()) return false;
      bool found = false;
      for (auto d = xcb_screen_allowed_depths_iterator(screen.data); d.rem; xcb_depth_next(&d)) {
        if (d.data->depth != depth) continue;
        for (auto v = xcb_depth_visuals_iterator(d.data); v.rem; xcb_visualtype_next(&v)) {
          if (v.data->visual_id != id) continue;
          image.true_color = v.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR;
          image.red_mask = v.data->red_mask; image.green_mask = v.data->green_mask; image.blue_mask = v.data->blue_mask;
          found = true;
        }
      }
      image.depth = depth;
      image.bits_per_pixel = 0;
      for (auto f = xcb_setup_pixmap_formats_iterator(setup); f.rem; xcb_format_next(&f)) {
        if (f.data->depth == depth) { image.bits_per_pixel = f.data->bits_per_pixel; image.scanline_pad = f.data->scanline_pad; }
      }
      if (setup->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST && setup->image_byte_order != XCB_IMAGE_ORDER_MSB_FIRST) return false;
      image.byte_order = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ? detail::X11ByteOrder::LeastSignificant : detail::X11ByteOrder::MostSignificant;
      const auto lane = [](std::uint32_t mask) { return mask && (mask >> std::countr_zero(mask)) == 255; };
      return found && image.true_color && (depth == 24 || depth == 32) && image.bits_per_pixel == 32 &&
             (image.scanline_pad == 8 || image.scanline_pad == 16 || image.scanline_pad == 32) &&
             lane(image.red_mask) && lane(image.green_mask) && lane(image.blue_mask) &&
             !(image.red_mask & image.green_mask) && !(image.red_mask & image.blue_mask) && !(image.green_mask & image.blue_mask) &&
             (depth == 32 || !((image.red_mask | image.green_mask | image.blue_mask) & 0xff000000U));
    }
    bool geometry(const xcb_get_window_attributes_reply_t &attributes, const xcb_get_geometry_reply_t &bounds) noexcept
    {
      if (attributes._class != XCB_WINDOW_CLASS_INPUT_OUTPUT || attributes.map_state != XCB_MAP_STATE_VIEWABLE ||
          bounds.root != target.root() || !bounds.width || !bounds.height) { fail(unavailable); return false; }
      if (!visual(attributes.visual, bounds.depth)) { fail(unsupported); return false; }
      if (target.kind() == core::SourceKind::Window &&
          (bounds.border_width > INT16_MAX || static_cast<std::uint32_t>(bounds.width) + 2U * bounds.border_width > UINT16_MAX ||
           static_cast<std::uint32_t>(bounds.height) + 2U * bounds.border_width > UINT16_MAX)) {
        fail(unsupported); return false;
      }
      image.allocation = target.kind() == core::SourceKind::Window ? core::Extent{bounds.width, bounds.height} :
                         core::Extent{target.region().width, target.region().height};
      image.drawable_extent = {bounds.width, bounds.height};
      image.region = target.kind() == core::SourceKind::Window ? core::PixelRegion{0, 0, bounds.width, bounds.height} : target.region();
      if (!native_region(image.region, image.drawable_extent)) { fail(unsupported); return false; }
      // Native widths are CARD16, so this exact 32-bpp/pad<=32 calculation fits
      // size_t even on 32-bit hosts. Scalar policy separately checks full row spans.
      image.stride = static_cast<std::size_t>(image.allocation.width) * 4;
      return true;
    }
    std::expected<void, core::AppError> initialize()
    {
      if (!target.valid() || !target.generation() || target.display_name().empty() || target.screen_number() < 0 ||
          (target.kind() != core::SourceKind::Window && target.kind() != core::SourceKind::Display)) return std::unexpected(unavailable);
      int screen{};
      connection.reset(xcb_connect(target.display_name().c_str(), &screen));
      if (!connection || xcb_connection_has_error(connection.get()) || screen != target.screen_number()) return std::unexpected(unavailable);
      auto *c = connection.get();
      // This watch precedes ALL source geometry/attribute queries, closing the
      // producer side of the catalog's checked second-resolve handoff.
      const std::uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
      Reply<xcb_generic_error_t> watch_error{xcb_request_check(c, xcb_change_window_attributes_checked(c, target.drawable(), XCB_CW_EVENT_MASK, &mask))};
      if (watch_error) { protocol_error(watch_error.get()); return std::unexpected(*pool.facts().error); }
      if (target.kind() == core::SourceKind::Window) {
        if (target.drawable() == target.root()) return std::unexpected(unavailable);
        const auto *extension = xcb_get_extension_data(c, &xcb_composite_id);
        if (!extension || !extension->present) return std::unexpected(unsupported);
        xcb_generic_error_t *error{};
        Reply<xcb_composite_query_version_reply_t> version{xcb_composite_query_version_reply(c, xcb_composite_query_version(c, 0, 4), &error)};
        Reply<xcb_generic_error_t> owned_error{error};
        if (!version || error || (!version->major_version && version->minor_version < 2)) return std::unexpected(unsupported);
      } else {
        if (target.drawable() != target.root()) return std::unexpected(unavailable);
        const auto *extension = xcb_get_extension_data(c, &xcb_randr_id);
        if (extension && extension->present) {
          xcb_generic_error_t *error{};
          Reply<xcb_randr_query_version_reply_t> version{xcb_randr_query_version_reply(c, xcb_randr_query_version(c, 1, 5), &error)};
          Reply<xcb_generic_error_t> owned_error{error};
          if (!version || error) return std::unexpected(operation_failed);
          if (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 2)) {
            auto flags = XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
                         XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY;
            if (version->major_version > 1 || version->minor_version >= 4) flags |= XCB_RANDR_NOTIFY_MASK_RESOURCE_CHANGE;
            Reply<xcb_generic_error_t> selected{xcb_request_check(c, xcb_randr_select_input_checked(c, target.root(), flags))};
            if (selected) return std::unexpected(operation_failed);
            randr_event = extension->first_event;
          }
        }
      }
      xcb_generic_error_t *error{};
      Reply<xcb_get_window_attributes_reply_t> attributes{xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, target.drawable()), &error)};
      Reply<xcb_generic_error_t> attributes_error{error};
      error = nullptr;
      Reply<xcb_get_geometry_reply_t> bounds{xcb_get_geometry_reply(c, xcb_get_geometry(c, target.drawable()), &error)};
      Reply<xcb_generic_error_t> bounds_error{error};
      if (!attributes || !bounds || attributes_error || bounds_error) {
        protocol_error(attributes_error ? attributes_error.get() : bounds_error.get());
        return std::unexpected(*pool.facts().error);
      }
      if (!geometry(*attributes, *bounds)) return std::unexpected(*pool.facts().error);
      events();
      if (pool.facts().lost) return std::unexpected(*pool.facts().error);
      initial = {detail::CpuPixelFormat::Bgra, image.allocation};
      if (auto negotiated = pool.negotiate(initial); !negotiated) return negotiated;
      initial_rev = pool.facts().revision;
      signal_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
      if (signal_fd < 0) return std::unexpected(core::AppError{core::AppErrorCode::PlatformOperationFailure, errno});
      return {};
    }
    /// @brief Replace a named resource only after the prior reply has retired.
    /// Composite storage includes the border: client pixels start at border_width.
    /// Both window and named-pixmap geometries must agree on that exact layout.
    /// Events arriving across any query invalidate this attempt before admission.
    bool rebuild() noexcept
    {
      dirty = false;
      const auto epoch = resource_epoch;
      auto *c = connection.get();
      if (pixmap) {
        const auto old = std::exchange(pixmap, 0);
        if (!checked(xcb_free_pixmap_checked(c, old))) return false;
      }
      const auto attributes = receive<xcb_get_window_attributes_reply_t>(xcb_get_window_attributes(c, target.drawable()).sequence);
      if (!attributes) return false;
      const auto bounds = receive<xcb_get_geometry_reply_t>(xcb_get_geometry(c, target.drawable()).sequence);
      if (!bounds || !geometry(*attributes, *bounds)) return false;
      if (target.kind() == core::SourceKind::Window) {
        if (!redirected) {
          if (!checked(xcb_composite_redirect_window_checked(c, target.drawable(), XCB_COMPOSITE_REDIRECT_AUTOMATIC))) return false;
          redirected = true;
        }
        pixmap = xcb_generate_id(c);
        if (!pixmap || pixmap == UINT32_MAX) { fail(operation_failed); return false; }
        if (!checked(xcb_composite_name_window_pixmap_checked(c, target.drawable(), pixmap))) return false;
        const auto named = receive<xcb_get_geometry_reply_t>(xcb_get_geometry(c, pixmap).sequence);
        if (!named) return false;
        const auto width = static_cast<std::uint32_t>(bounds->width) + 2U * bounds->border_width;
        const auto height = static_cast<std::uint32_t>(bounds->height) + 2U * bounds->border_width;
        if (dirty || epoch != resource_epoch) return true;
        if (named->root != target.root() || named->depth != bounds->depth || named->width != width || named->height != height || named->border_width) {
          fail(unsupported); return false;
        }
        image.drawable_extent = {named->width, named->height};
        image.region = {bounds->border_width, bounds->border_width, bounds->width, bounds->height};
        if (!native_region(image.region, image.drawable_extent)) { fail(unsupported); return false; }
      }
      if (dirty || epoch != resource_epoch) return true;
      if (auto result = pool.negotiate({detail::CpuPixelFormat::Bgra, image.allocation}); !result) { fail(result.error()); return false; }
      return true;
    }
    void notify() noexcept
    {
      if (!wake) return;
      try { wake(); } catch (...) {
        fail(operation_failed);
        wake = {}; // A failed host callback is never reentered during terminal retirement.
      }
    }
    void run() noexcept
    {
      std::uint64_t notified_epoch{};
      while (active()) {
        events();
        if (!active()) break;
        if (dirty && !rebuild()) break;
        if (!active()) break;
        pool.service_storage();
        const auto facts = pool.facts();
        if (facts.epoch != notified_epoch) { notified_epoch = facts.epoch; notify(); }
        if (dirty || !active()) continue;
        if (facts.storage_ready_revision != facts.revision) { wait_socket(16); continue; }
        const detail::X11RequestStamp stamp{target.generation(), resource_epoch, facts.revision};
        const auto request_facts = image;
        const auto r = image.region;
        const auto drawable = target.kind() == core::SourceKind::Window ? pixmap : target.drawable();
        auto reply = receive<xcb_get_image_reply_t>(xcb_get_image(connection.get(), XCB_IMAGE_FORMAT_Z_PIXMAP, drawable,
                                                   static_cast<std::int16_t>(r.x), static_cast<std::int16_t>(r.y),
                                                   static_cast<std::uint16_t>(r.width), static_cast<std::uint16_t>(r.height), UINT32_MAX).sequence);
        if (!reply) break;
        // receive drains after poll. Check again before reservation because queued
        // Configure may preserve allocation while retiring this named pixmap epoch.
        events();
        if (!detail::x11_request_current(stamp, target.generation(), resource_epoch, pool.facts(), target.valid())) continue;
        const std::size_t payload_units = reply->length;
        if (reply->depth != request_facts.depth || payload_units > std::numeric_limits<std::size_t>::max() / 4) { fail(unsupported); break; }
        std::optional<detail::CpuFrameWrite> writer;
        bool copied = false;
        {
          const auto payload = std::span<const std::byte>{reinterpret_cast<const std::byte *>(reply.get() + 1), payload_units * 4};
          auto plan = detail::inspect_x11_image(request_facts, payload);
          if (!plan) { fail(unsupported); break; }
          writer = pool.begin_write(stamp.allocation_revision, request_facts.allocation, Clock::now());
          if (writer) copied = detail::copy_x11_image(*plan, writer->bytes(), writer->row_pitch());
        }
        reply.reset(); // End every native byte borrow before publication consumes writer.
        events();
        if (writer && copied && detail::x11_request_current(stamp, target.generation(), resource_epoch, pool.facts(), target.valid())) {
          bool published = false;
          {
            std::lock_guard lock{publication_mutex};
            if (active()) published = writer->commit();
          }
          if (published) notify();
        } else if (writer && !copied) fail(unsupported);
        writer.reset();
        wait_socket(16);
      }
      notify();
      retire();
      finished.store(true, std::memory_order_release);
    }
  };

  X11Capture::X11Capture(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
  X11Capture::~X11Capture()
  {
    if (impl_->worker.joinable()) {
      if (!impl_->finished.load(std::memory_order_acquire)) std::terminate();
      impl_->worker.join();
    }
  }
  std::expected<std::unique_ptr<X11Capture>, core::AppError> X11Capture::create(X11SourceTarget target, std::function<void()> wake)
  {
    try {
      auto impl = std::make_unique<Impl>(std::move(target), std::move(wake));
      if (auto result = impl->initialize(); !result) return std::unexpected(result.error());
      return std::unique_ptr<X11Capture>{new X11Capture{std::move(impl)}};
    } catch (...) { return std::unexpected(construction_failed); }
  }
  std::expected<void, core::AppError> X11Capture::start() noexcept
  {
    if (impl_->started || impl_->finished.load(std::memory_order_acquire) || impl_->pool.facts().stopping) return std::unexpected(state_lost);
    if (!impl_->target.valid()) return std::unexpected(unavailable);
    impl_->activation = Clock::now();
    try {
      impl_->worker = std::thread{[state = impl_.get()] { state->run(); }};
      impl_->started = true;
      return {};
    } catch (...) {
      impl_->activation.reset();
      impl_->pool.fail(construction_failed);
      impl_->retire();
      impl_->finished.store(true, std::memory_order_release);
      return std::unexpected(construction_failed);
    }
  }
  X11CaptureProgress X11Capture::progress(Clock::time_point now) noexcept
  {
    auto frame = impl_->pool.facts();
    if (detail::x11_first_frame_expired(impl_->activation, now, frame)) {
      bool expired = false;
      {
        std::lock_guard lock{impl_->publication_mutex};
        // First publication may have won since the initial snapshot. Only this
        // current observation can close admission for the first-frame deadline.
        frame = impl_->pool.facts();
        expired = detail::x11_first_frame_expired(impl_->activation, now, frame);
        if (expired) {
          impl_->pool.fail(state_lost);
          frame = impl_->pool.facts();
        }
      }
      if (expired) impl_->signal();
    }
    const bool finished = impl_->finished.load(std::memory_order_acquire);
    if (finished) {
      // The first snapshot can precede terminal publication. Acquire retirement
      // before rereading its final facts, so quiescence cannot hide the last cause
      // while also removing the caller's scheduled wake.
      frame = impl_->pool.facts();
      if (impl_->worker.joinable()) impl_->worker.join();
    }
    X11CaptureProgress result{frame, frame.epoch != impl_->seen_epoch || finished != impl_->seen_finished, finished, {}};
    impl_->seen_epoch = frame.epoch;
    impl_->seen_finished = finished;
    if (!finished && impl_->activation) {
      result.next_wake = now + std::chrono::milliseconds{16};
      if (!frame.sequence && !frame.stopping && !frame.lost)
        result.next_wake = std::min(*result.next_wake, *impl_->activation + std::chrono::seconds{10});
    }
    return result;
  }
  std::optional<LinuxCapturedFrame> X11Capture::take_latest() noexcept { return impl_->pool.take_latest(); }
  std::expected<void, core::AppError> X11Capture::acknowledge_allocation_revision(std::uint64_t revision) noexcept
  {
    auto result = impl_->pool.acknowledge(revision);
    if (result) impl_->signal();
    return result;
  }
  void X11Capture::request_stop() noexcept
  {
    impl_->pool.request_stop();
    if (!impl_->started) {
      impl_->retire();
      impl_->finished.store(true, std::memory_order_release);
    } else impl_->signal();
  }
  detail::CpuFormat X11Capture::initial_format() const noexcept { return impl_->initial; }
  std::uint64_t X11Capture::initial_revision() const noexcept { return impl_->initial_rev; }
}
