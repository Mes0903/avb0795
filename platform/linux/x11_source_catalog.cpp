/// @file
/// @brief Native observations are committed only after watched destruction is drained.
/// The scalar admission policy below is the sole source of descriptor identities;
/// XCB code supplies checked facts and retires observations before publication.
#include "platform/linux/x11_source_catalog.hpp"

#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/randr.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace avb::linux_platform {
  namespace {
    constexpr core::AppError unavailable{core::AppErrorCode::SourceUnavailable};
    constexpr core::AppError catalog_failure{core::AppErrorCode::SourceCatalogFailure};
    constexpr core::AppError terminal_failure{core::AppErrorCode::RuntimeStateLost};

    bool admitted(const detail::X11CatalogProbe &probe, const detail::X11SourceObservation &observation, std::span<const std::uint32_t> owned) noexcept
    {
      if (!observation.drawable_valid || !observation.drawable ||
          !observation.extent.width || !observation.extent.height) return false;
      const auto capabilities = detail::x11_catalog_capabilities(probe);
      if (observation.kind == core::SourceKind::Window) {
        return capabilities.window_capture && observation.ewmh_client &&
               observation.drawable != probe.root && !observation.monitor_name &&
               std::ranges::find(owned, observation.drawable) == owned.end();
      }
      if (observation.kind != core::SourceKind::Display || !capabilities.display_capture ||
          observation.drawable != probe.root || observation.x < 0 || observation.y < 0) return false;
      const core::PixelRegion region{static_cast<std::uint32_t>(observation.x),
                                     static_cast<std::uint32_t>(observation.y),
                                     observation.extent.width, observation.extent.height};
      if (!core::valid_region(region, probe.root_extent)) return false;
      // Name zero means the whole root, not an anonymous monitor whose position
      // could be reused by another output after a layout change.
      return observation.monitor_name ||
             (region.x == 0 && region.y == 0 && observation.extent == probe.root_extent);
    }

    struct FreeReply {
      void operator()(void *value) const noexcept { std::free(value); }
    };
    template <class T>
    using Reply = std::unique_ptr<T, FreeReply>;

    /// @brief Retire both success replies and checked protocol errors with one owner.
    /// A null result never supplies native facts. Connection failure is additionally
    /// checked at each publication barrier, where it invalidates the whole catalog.
    template <class T, class Cookie, class Function>
    Reply<T> checked_reply(xcb_connection_t *connection, Cookie cookie, Function function)
    {
      xcb_generic_error_t *error{};
      Reply<T> reply{function(connection, cookie, &error)};
      const Reply<xcb_generic_error_t> error_owner{error};
      if (error) return {};
      return reply;
    }
  }    // namespace

  bool X11SourceTarget::valid() const noexcept
  {
    return live_ && live_->load(std::memory_order_acquire);
  }

  namespace detail {
    std::expected<std::uint64_t, core::AppError> next_x11_generation(std::uint64_t last_issued) noexcept
    {
      if (last_issued == std::numeric_limits<std::uint64_t>::max()) return std::unexpected(terminal_failure);
      return last_issued + 1;
    }

    core::PlatformCapabilities x11_catalog_capabilities(const X11CatalogProbe &probe) noexcept
    {
      core::PlatformCapabilities result;
      result.window_capture = probe.composite_available &&
                              (probe.composite_major > 0 || probe.composite_minor >= 2) &&
                              probe.ewmh_client_list_valid;
      result.display_capture = probe.root && probe.root_drawable_valid &&
                               probe.root_extent.width && probe.root_extent.height;
      result.window_opacity = false;
      return result;
    }

    X11CatalogState::X11CatalogState(std::string display_name, int screen_number)
        : display_name_(std::move(display_name)), screen_number_(screen_number) {}
    X11CatalogState::~X11CatalogState() { invalidate_all(); }

    void X11CatalogState::invalidate_all() noexcept
    {
      for (auto &record : records_) record.target.live_->store(false, std::memory_order_release);
      records_.clear();
    }

    void X11CatalogState::destroy(std::uint32_t drawable) noexcept
    {
      std::erase_if(records_, [drawable](auto &record) {
        if (record.target.drawable() != drawable) return false;
        record.target.live_->store(false, std::memory_order_release);
        return true;
      });
    }

    void X11CatalogState::invalidate_displays() noexcept
    {
      std::erase_if(records_, [](auto &record) {
        if (record.target.kind() != core::SourceKind::Display) return false;
        record.target.live_->store(false, std::memory_order_release);
        return true;
      });
    }

    std::expected<std::vector<core::SourceDescriptor>, core::AppError> X11CatalogState::refresh(
      const X11CatalogProbe &probe, std::span<const X11SourceObservation> observations, std::span<const std::uint32_t> owned)
    {
      if (exhausted_) return std::unexpected(terminal_failure);
      try {
        std::vector<Record> next;
        std::vector<core::SourceDescriptor> result;
        next.reserve(observations.size());
        result.reserve(observations.size());
        for (const auto &observation : observations) {
          if (!admitted(probe, observation, owned)) continue;
          const auto same_key = [&](const Record &record) {
            return record.target.kind() == observation.kind &&
                   record.target.drawable() == observation.drawable &&
                   record.target.monitor_name() == observation.monitor_name;
          };
          if (std::ranges::find_if(next, same_key) != next.end()) continue;
          auto existing = std::ranges::find_if(records_, same_key);
          const core::PixelRegion region{
            observation.kind == core::SourceKind::Window ? 0U : static_cast<std::uint32_t>(observation.x),
            observation.kind == core::SourceKind::Window ? 0U : static_cast<std::uint32_t>(observation.y),
            observation.extent.width, observation.extent.height};
          // Window resize retains its observation, while a changed display region
          // interrupts capture rather than silently switching the meaning of pixels.
          if (existing != records_.end() && (existing->target.root() != probe.root ||
                                             (observation.kind == core::SourceKind::Display && existing->target.region() != region))) {
            existing->target.live_->store(false, std::memory_order_release);
            existing = records_.end();
          }
          Record record;
          if (existing != records_.end()) {
            record.target = existing->target;
            record.descriptor.id = existing->descriptor.id;
          }
          else {
            const auto generation = next_x11_generation(last_issued_);
            if (!generation) {
              exhausted_ = true;
              invalidate_all();
              return std::unexpected(generation.error());
            }
            last_issued_ = *generation;
            record.target.live_ = std::make_shared<std::atomic_bool>(true);
            record.target.generation_ = *generation;
            record.target.kind_ = observation.kind;
            record.target.drawable_ = observation.drawable;
            record.target.monitor_name_ = observation.monitor_name;
            record.target.root_ = probe.root;
            record.target.display_name_ = display_name_;
            record.target.screen_number_ = screen_number_;
            record.descriptor.id.value = "x11-observation:" + std::to_string(*generation);
          }
          record.target.region_ = region;
          record.descriptor.kind = observation.kind;
          record.descriptor.display_label = observation.display_label;
          record.descriptor.extent = observation.extent;
          record.descriptor.native_handle_text = std::to_string(observation.drawable);
          result.push_back(record.descriptor);
          next.push_back(std::move(record));
        }
        for (auto &old : records_) {
          const auto retained = std::ranges::find_if(next, [&](const auto &record) {
            return old.descriptor.id == record.descriptor.id;
          });
          if (retained == next.end()) old.target.live_->store(false, std::memory_order_release);
        }
        records_ = std::move(next);
        return result;
      } catch (...) {
        invalidate_all();
        return std::unexpected(catalog_failure);
      }
    }

    std::expected<X11SourceTarget, core::AppError> X11CatalogState::resolve(const core::SourceId &id) const
    {
      if (exhausted_) return std::unexpected(terminal_failure);
      const auto record = std::ranges::find_if(records_, [&](const auto &entry) { return entry.descriptor.id == id; });
      if (record == records_.end() || !record->target.valid()) return std::unexpected(unavailable);
      try {
        return record->target;
      } catch (...) {
        return std::unexpected(catalog_failure);
      }
    }
  }    // namespace detail

  /// @brief Single-thread observer; watches outlive catalog rows until native Destroy.
  /// Replies and events are connection-local. Each scan fences its reads and filters
  /// any candidate destroyed during that scan, including a reused XID which already
  /// supplied a newer reply. Such a candidate is admitted only by a later fresh scan.
  struct X11SourceCatalog::Impl {
    struct Disconnect {
      void operator()(xcb_connection_t *connection) const noexcept { xcb_disconnect(connection); }
    };
    struct ScanEvents {
      std::unordered_set<std::uint32_t> destroyed;
      bool displays_changed{};
    };
    std::unique_ptr<xcb_connection_t, Disconnect> connection;
    detail::X11CatalogState state;
    detail::X11CatalogProbe probe;
    std::unordered_set<std::uint32_t> watched;
    std::vector<std::uint32_t> owned;
    xcb_atom_t supporting_wm{}, supported{}, client_list{}, window_name{}, utf8_string{};
    std::uint8_t randr_first_event{};
    bool randr_monitors{}, failed{};

    Impl(std::unique_ptr<xcb_connection_t, Disconnect> connection_owner, std::string display, int screen)
        : connection(std::move(connection_owner)), state(std::move(display), screen) {}
    ~Impl() { state.invalidate_all(); }

    std::expected<void, core::AppError> fail(std::int64_t detail = 0) noexcept
    {
      failed = true;
      state.invalidate_all();
      probe = {};
      return std::unexpected(core::AppError{core::AppErrorCode::SourceCatalogFailure, detail});
    }

    /// @brief Establish a server processing boundary before consuming queued events.
    /// Waiting for a reply makes preceding events available to this connection's
    /// queue; a mere flush/poll could miss Destroy still in transit. Events beyond
    /// the fence are also safe to retire conservatively, never to reconnect by XID.
    std::expected<void, core::AppError> barrier(ScanEvents &events)
    {
      if (failed) return std::unexpected(catalog_failure);
      const auto reply = checked_reply<xcb_get_input_focus_reply_t>(connection.get(),
                                                                    xcb_get_input_focus(connection.get()), xcb_get_input_focus_reply);
      if (!reply) return fail(xcb_connection_has_error(connection.get()));
      while (Reply<xcb_generic_event_t> event{xcb_poll_for_event(connection.get())}) {
        // Synthetic client messages must not retire a genuine lifetime watch.
        if (event->response_type & 0x80) continue;
        const auto type = event->response_type;
        if (type == 0) return fail(reinterpret_cast<xcb_generic_error_t *>(event.get())->error_code);
        if (type == XCB_DESTROY_NOTIFY) {
          const auto window = reinterpret_cast<xcb_destroy_notify_event_t *>(event.get())->window;
          state.destroy(window);
          watched.erase(window);
          std::erase(owned, window);
          events.destroyed.insert(window);
          if (window == probe.root) return fail();
        }
        else if ((type == XCB_CONFIGURE_NOTIFY &&
                  reinterpret_cast<xcb_configure_notify_event_t *>(event.get())->window == probe.root) ||
                 (randr_first_event && (type == randr_first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
                                        type == randr_first_event + XCB_RANDR_NOTIFY))) {
          state.invalidate_displays();
          events.displays_changed = true;
        }
      }
      if (const auto error = xcb_connection_has_error(connection.get())) return fail(error);
      return {};
    }

    bool watch(std::uint32_t window)
    {
      if (watched.contains(window)) return true;
      const std::uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
      const Reply<xcb_generic_error_t> error{xcb_request_check(connection.get(),
                                                               xcb_change_window_attributes_checked(connection.get(), window, XCB_CW_EVENT_MASK, &mask))};
      if (error || xcb_connection_has_error(connection.get())) return false;
      watched.insert(window);
      return true;
    }

    xcb_atom_t atom(std::string_view name)
    {
      const auto reply = checked_reply<xcb_intern_atom_reply_t>(connection.get(),
                                                                xcb_intern_atom(connection.get(), true, static_cast<std::uint16_t>(name.size()), name.data()),
                                                                xcb_intern_atom_reply);
      return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    }

    Reply<xcb_get_property_reply_t> property(std::uint32_t window, xcb_atom_t name, xcb_atom_t type, std::uint32_t max_units = 65536)
    {
      if (!name || !type) return {};
      auto reply = checked_reply<xcb_get_property_reply_t>(connection.get(),
                                                           xcb_get_property(connection.get(), false, window, name, type, 0, max_units), xcb_get_property_reply);
      if (!reply || reply->type != type || reply->bytes_after ||
          (reply->format != 8 && reply->format != 16 && reply->format != 32) ||
          static_cast<std::uint64_t>(reply->value_len) * (reply->format / 8) >
            static_cast<std::uint64_t>(reply->length) * 4) return {};
      return reply;
    }

    std::optional<std::vector<std::uint32_t>> property_words(std::uint32_t window, xcb_atom_t name, xcb_atom_t type)
    {
      const auto reply = property(window, name, type);
      if (!reply || reply->format != 32 ||
          static_cast<std::uint64_t>(reply->value_len) * 4 != static_cast<std::uint64_t>(xcb_get_property_value_length(reply.get()))) return {};
      std::vector<std::uint32_t> result(reply->value_len);
      if (!result.empty()) std::memcpy(result.data(), xcb_get_property_value(reply.get()), result.size() * sizeof(std::uint32_t));
      return result;
    }

    std::string title(std::uint32_t window)
    {
      auto reply = property(window, window_name, utf8_string, 1024);
      if (!reply || reply->format != 8) reply = property(window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 1024);
      if (!reply || reply->format != 8 || !reply->value_len) return "X11 window " + std::to_string(window);
      const auto *text = static_cast<const char *>(xcb_get_property_value(reply.get()));
      return std::string{text, reply->value_len};
    }

    std::optional<core::Extent> geometry(std::uint32_t window, bool require_mapped)
    {
      const auto attributes = checked_reply<xcb_get_window_attributes_reply_t>(connection.get(),
                                                                               xcb_get_window_attributes(connection.get(), window), xcb_get_window_attributes_reply);
      const auto bounds = checked_reply<xcb_get_geometry_reply_t>(connection.get(),
                                                                  xcb_get_geometry(connection.get(), window), xcb_get_geometry_reply);
      if (!attributes || !bounds || attributes->_class != XCB_WINDOW_CLASS_INPUT_OUTPUT ||
          (require_mapped && attributes->map_state != XCB_MAP_STATE_VIEWABLE) ||
          bounds->root != probe.root || !bounds->width || !bounds->height) return {};
      return core::Extent{bounds->width, bounds->height};
    }

    /// @brief Require the EWMH root/support-window handshake and advertised list.
    /// Atom existence alone is not a running compliant window manager. A valid
    /// empty client property remains a valid capability with no selectable windows.
    std::optional<std::vector<std::uint32_t>> clients()
    {
      // Resolve existing atoms each time so starting/replacing a WM after catalog
      // construction can restore discovery without fabricating server properties.
      supporting_wm = atom("_NET_SUPPORTING_WM_CHECK");
      supported = atom("_NET_SUPPORTED");
      client_list = atom("_NET_CLIENT_LIST");
      window_name = atom("_NET_WM_NAME");
      utf8_string = atom("UTF8_STRING");
      const auto manager = property_words(probe.root, supporting_wm, XCB_ATOM_WINDOW);
      if (!manager || manager->size() != 1 || !manager->front() || manager->front() == probe.root) return {};
      const auto self = property_words(manager->front(), supporting_wm, XCB_ATOM_WINDOW);
      if (!self || *self != *manager) return {};
      const auto name = property(manager->front(), window_name, utf8_string, 1024);
      if (!name || name->format != 8 || !name->value_len) return {};
      const auto support = property_words(probe.root, supported, XCB_ATOM_ATOM);
      if (!support || std::ranges::find(*support, client_list) == support->end()) return {};
      return property_words(probe.root, client_list, XCB_ATOM_WINDOW);
    }

    /// @brief Prefer stable RandR monitor atoms; whole-root capture remains a fallback.
    /// Malformed/empty monitor data cannot create a partial or out-of-root target;
    /// scalar admission independently checks every rectangle before publication.
    void displays(std::vector<detail::X11SourceObservation> &observations)
    {
      if (!probe.root_drawable_valid) return;
      if (randr_monitors) {
        const auto reply = checked_reply<xcb_randr_get_monitors_reply_t>(connection.get(),
                                                                         xcb_randr_get_monitors(connection.get(), probe.root, true), xcb_randr_get_monitors_reply);
        if (reply) {
          const auto *cursor = reinterpret_cast<const std::byte *>(reply.get() + 1);
          std::size_t remaining = static_cast<std::size_t>(reply->length) * 4;
          for (std::uint32_t index = 0; index < reply->nMonitors; ++index) {
            if (remaining < sizeof(xcb_randr_monitor_info_t)) break;
            xcb_randr_monitor_info_t monitor{};
            std::memcpy(&monitor, cursor, sizeof(monitor));
            const auto bytes = sizeof(monitor) + static_cast<std::size_t>(monitor.nOutput) * sizeof(xcb_randr_output_t);
            if (remaining < bytes) break;
            if (monitor.name) {
              auto label = std::string{"X11 monitor "} + std::to_string(monitor.name);
              const auto name = checked_reply<xcb_get_atom_name_reply_t>(connection.get(),
                                                                         xcb_get_atom_name(connection.get(), monitor.name), xcb_get_atom_name_reply);
              if (name && name->name_len && name->name_len <= static_cast<std::uint64_t>(name->length) * 4)
                label.assign(xcb_get_atom_name_name(name.get()), name->name_len);
              detail::X11SourceObservation observation{
                .kind = core::SourceKind::Display, .drawable = probe.root, .monitor_name = monitor.name, .display_label = std::move(label), .x = monitor.x, .y = monitor.y, .extent = {monitor.width, monitor.height}, .drawable_valid = true};
              if (admitted(probe, observation, {})) observations.push_back(std::move(observation));
            }
            cursor += bytes;
            remaining -= bytes;
          }
        }
      }
      if (observations.empty()) observations.push_back({.kind = core::SourceKind::Display, .drawable = probe.root, .display_label = "X11 root display", .extent = probe.root_extent, .drawable_valid = true});
    }

    std::expected<void, core::AppError> initialize(int screen_number)
    {
      const auto *setup = xcb_get_setup(connection.get());
      if (!setup) return fail();
      auto screen = xcb_setup_roots_iterator(setup);
      for (int index = 0; index < screen_number && screen.rem; ++index) xcb_screen_next(&screen);
      if (!screen.rem) return fail();
      probe.root = screen.data->root;
      if (!watch(probe.root)) return fail();
      const auto *composite = xcb_get_extension_data(connection.get(), &xcb_composite_id);
      if (composite && composite->present) {
        const auto version = checked_reply<xcb_composite_query_version_reply_t>(connection.get(),
                                                                                xcb_composite_query_version(connection.get(), 0, 4), xcb_composite_query_version_reply);
        if (version) {
          probe.composite_available = true;
          probe.composite_major = version->major_version;
          probe.composite_minor = version->minor_version;
        }
      }
      const auto *randr = xcb_get_extension_data(connection.get(), &xcb_randr_id);
      if (randr && randr->present) {
        const auto version = checked_reply<xcb_randr_query_version_reply_t>(connection.get(),
                                                                            xcb_randr_query_version(connection.get(), 1, 5), xcb_randr_query_version_reply);
        if (version && (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 2))) {
          auto mask = XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
                      XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY;
          // Monitor definitions can change without changing root dimensions or
          // CRTC placement. RandR 1.4 resource notifications cover that lifetime.
          if (version->major_version > 1 || version->minor_version >= 4)
            mask |= XCB_RANDR_NOTIFY_MASK_RESOURCE_CHANGE;
          const Reply<xcb_generic_error_t> error{xcb_request_check(connection.get(),
                                                                   xcb_randr_select_input_checked(connection.get(), probe.root, mask))};
          if (!error) {
            randr_first_event = randr->first_event;
            randr_monitors = version->major_version > 1 || version->minor_version >= 5;
          }
        }
      }
      ScanEvents events;
      return barrier(events);
    }

    std::expected<std::vector<core::SourceDescriptor>, core::AppError> enumerate()
    {
      ScanEvents previous;
      if (auto ready = barrier(previous); !ready) return std::unexpected(ready.error());
      const auto root_extent = geometry(probe.root, false);
      probe.root_drawable_valid = root_extent.has_value();
      probe.root_extent = root_extent.value_or(core::Extent{});
      auto windows = clients();
      probe.ewmh_client_list_valid = windows.has_value();
      std::vector<detail::X11SourceObservation> observations;
      displays(observations);
      if (detail::x11_catalog_capabilities(probe).window_capture) {
        for (const auto window : *windows) {
          if (!window || window == probe.root || std::ranges::find(owned, window) != owned.end()) continue;
          if (!watch(window)) continue;
          const auto extent = geometry(window, true);
          if (!extent) continue;
          observations.push_back({.kind = core::SourceKind::Window, .drawable = window, .display_label = title(window), .extent = *extent, .drawable_valid = true, .ewmh_client = true});
        }
      }
      ScanEvents during;
      if (auto ready = barrier(during); !ready) return std::unexpected(ready.error());
      // Never reuse facts queried across a Destroy. Even if a new resource now
      // has that number, a future scan must first establish its own checked watch.
      std::erase_if(observations, [&](const auto &observation) {
        return during.destroyed.contains(observation.drawable) ||
               (during.displays_changed && observation.kind == core::SourceKind::Display);
      });
      return state.refresh(probe, observations, owned);
    }
  };

  X11SourceCatalog::X11SourceCatalog(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
  X11SourceCatalog::~X11SourceCatalog() = default;

  std::expected<std::unique_ptr<X11SourceCatalog>, core::AppError> X11SourceCatalog::create()
  {
    try {
      // Freeze DISPLAY for the future producer connection instead of borrowing
      // mutable process environment or an unrelated window library connection.
      const auto *environment = std::getenv("DISPLAY");
      if (!environment || !*environment) return std::unexpected(catalog_failure);
      std::string display{environment};
      int screen{};
      std::unique_ptr<xcb_connection_t, Impl::Disconnect> connection{xcb_connect(display.c_str(), &screen)};
      if (!connection || xcb_connection_has_error(connection.get())) return std::unexpected(catalog_failure);
      auto impl = std::make_unique<Impl>(std::move(connection), std::move(display), screen);
      if (auto initialized = impl->initialize(screen); !initialized) return std::unexpected(initialized.error());
      if (auto initial = impl->enumerate(); !initial) return std::unexpected(initial.error());
      return std::unique_ptr<X11SourceCatalog>{new X11SourceCatalog{std::move(impl)}};
    } catch (...) {
      return std::unexpected(catalog_failure);
    }
  }

  std::expected<std::vector<core::SourceDescriptor>, core::AppError> X11SourceCatalog::enumerate()
  {
    try {
      return impl_->enumerate();
    } catch (...) {
      impl_->state.invalidate_all();
      return std::unexpected(catalog_failure);
    }
  }

  std::expected<X11SourceTarget, core::AppError> X11SourceCatalog::resolve(const core::SourceId &id)
  {
    const auto refreshed = enumerate();
    if (!refreshed) return std::unexpected(refreshed.error());
    return impl_->state.resolve(id);
  }

  core::PlatformCapabilities X11SourceCatalog::capabilities() const noexcept
  {
    return detail::x11_catalog_capabilities(impl_->probe);
  }

  std::expected<void, core::AppError> X11SourceCatalog::register_owned_window(std::uint32_t drawable)
  {
    try {
      Impl::ScanEvents previous;
      if (auto ready = impl_->barrier(previous); !ready) return ready;
      if (!drawable || drawable == impl_->probe.root) return std::unexpected(unavailable);
      const bool observed = impl_->watch(drawable) && impl_->geometry(drawable, false).has_value();
      Impl::ScanEvents during;
      if (auto ready = impl_->barrier(during); !ready) return ready;
      if (!observed || during.destroyed.contains(drawable)) return std::unexpected(unavailable);
      if (std::ranges::find(impl_->owned, drawable) == impl_->owned.end()) impl_->owned.push_back(drawable);
      impl_->state.destroy(drawable);
      return {};
    } catch (...) {
      impl_->state.invalidate_all();
      return std::unexpected(catalog_failure);
    }
  }
}    // namespace avb::linux_platform
