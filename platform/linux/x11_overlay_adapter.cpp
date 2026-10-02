/// @file
/// @brief X11 server transactions, asynchronous WM targets and bounded replica input.
#include "platform/linux/x11_overlay_adapter.hpp"
#include "platform/linux/x11_controller_window.hpp"
#include "platform/linux/detail/overlay_policy.hpp"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <thread>
#include <unordered_map>
#include <vector>
namespace avb::linux_platform {
  namespace {
    /// @brief Narrow main-thread protocol scope, preserving unrelated queued events/errors.
    /// GLFW polling and setters with their own traps never execute inside this scope.
    class Trap {
    public:
      explicit Trap(Display *d) : display(d)
      {
        if (current) std::terminate();
        XSync(d, False);
        first = NextRequest(d);
        previous = XSetErrorHandler(handler);
        current = this;
      }
      ~Trap() { finish(); }
      bool finish() noexcept
      {
        if (current == this) {
          last = NextRequest(display);
          XSync(display, False);
          XSetErrorHandler(previous);
          current = nullptr;
        }
        return error == 0;
      }
      int error{};

    private:
      static int handler(Display *d, XErrorEvent *e) noexcept
      {
        auto *t = current;
        if (t && d == t->display && e->serial >= t->first && e->serial < (t->last ? t->last : NextRequest(d))) {
          if (!t->error) t->error = e->error_code;
          return 0;
        }
        try {
          return t && t->previous ? t->previous(d, e) : 0;
        } catch (...) {
          return 0;
        }
      }
      Display *display;
      unsigned long first{}, last{};
      XErrorHandler previous{};
      static inline Trap *current{};
    };
    /// @brief Protocol-value snapshot, preserving absence, type and width across LP64 Xlib.
    /// Xlib format32 uses native longs; lower protocol bits are normalized before
    /// comparisons and reconstructed as native longs for XChangeProperty.
    struct Property {
      Atom type{};
      int format{};
      std::vector<unsigned char> bytes;
      friend bool operator==(const Property &, const Property &) = default;
      static std::optional<Property> read(Display *d, Window w, Atom atom)
      {
        Atom type{};
        int format{};
        unsigned long count{}, remaining{};
        unsigned char *raw{};
        const auto status = XGetWindowProperty(d, w, atom, 0, 16384, False, AnyPropertyType, &type, &format, &count, &remaining, &raw);
        struct Free {
          unsigned char *p;
          ~Free()
          {
            if (p) XFree(p);
          }
        } cleanup{raw};
        if (status != Success || remaining || count > 65536 || (type != None && format != 8 && format != 16 && format != 32)) return {};
        Property p{type, format, {}};
        if (type == None) return p;
        p.bytes.resize(count * static_cast<unsigned>(format / 8));
        if (format == 32)
          for (unsigned long i = 0; i < count; ++i) {
            const std::uint32_t value = static_cast<std::uint32_t>(reinterpret_cast<unsigned long *>(raw)[i]);
            std::memcpy(p.bytes.data() + i * 4, &value, 4);
          }
        else if (!p.bytes.empty())
          std::memcpy(p.bytes.data(), raw, p.bytes.size());
        return p;
      }
      [[nodiscard]] std::vector<unsigned long> words() const
      {
        std::vector<unsigned long> values(bytes.size() / 4);
        for (std::size_t i = 0; i < values.size(); ++i) {
          std::uint32_t v{};
          std::memcpy(&v, bytes.data() + i * 4, 4);
          values[i] = v;
        }
        return values;
      }
      void write(Display *d, Window w, Atom atom) const
      {
        if (type == None) {
          XDeleteProperty(d, w, atom);
          return;
        }
        if (format == 32) {
          const auto v = words();
          XChangeProperty(d, w, atom, type, format, PropModeReplace, reinterpret_cast<const unsigned char *>(v.data()), static_cast<int>(v.size()));
        }
        else
          XChangeProperty(d, w, atom, type, format, PropModeReplace, bytes.data(), static_cast<int>(bytes.size() / (format / 8)));
      }
      bool one(Atom expected, unsigned long value) const
      {
        if (type != expected || format != 32 || bytes.size() != 4) return false;
        std::uint32_t v{};
        std::memcpy(&v, bytes.data(), 4);
        return v == static_cast<std::uint32_t>(value);
      }
    };
    core::AppError operation_error(int native = 0) { return {core::AppErrorCode::PlatformOperationFailure, native ? std::optional<std::int64_t>(native) : std::nullopt}; }
    core::Extent client_extent(GLFWwindow *w)
    {
      int x{}, y{};
      glfwGetWindowSize(w, &x, &y);
      return {static_cast<unsigned>(std::max(0, x)), static_cast<unsigned>(std::max(0, y))};
    }
    float scale(GLFWwindow *w)
    {
      float x{}, y{};
      glfwGetWindowContentScale(w, &x, &y);
      return std::max(x, y);
    }
  }    // namespace
  /// @brief One borrowed-window attachment and its owner-thread-only callback/state graph.
  /// Settings, native input Shape and accepted WM targets commit together. Selection
  /// temporarily changes input/opacity, with fixed mapping and one terminal fact;
  /// predecessor callbacks are contained and forwarded once without host user storage.
  struct X11OverlayAdapter::Impl {
    GLFWwindow *window{};
    Display *display{};
    Window xid{}, root{};
    std::thread::id thread{std::this_thread::get_id()};
    core::ReplicaSettings settings{};
    core::PlatformCapabilities caps{};
    detail::SelectionMapping selection;
    X11OverlayEvents events;
    bool input_empty{}, above_target{}, gesturing{}, selection_override{}, attached{}, probe_state_lost{};
    double scroll_remainder{};
    Atom motif{}, opacity{}, state{}, above{}, normal{}, moveresize_atom{};
    // Native shape ownership starts at default; only this adapter changes input Shape.
    struct Snapshot {
      std::array<Property, 4> properties;
      bool mapped{}, empty{}, above{};
    } saved;
    GLFWmousebuttonfun previous_button{};
    GLFWcursorposfun previous_cursor{};
    GLFWscrollfun previous_scroll{};
    GLFWkeyfun previous_key{};
    GLFWwindowfocusfun previous_focus{};
    GLFWwindowsizefun previous_size{};
    GLFWframebuffersizefun previous_framebuffer{};
    GLFWwindowclosefun previous_close{};
    static inline std::unordered_map<GLFWwindow *, Impl *> owners;
    void check_thread() const noexcept
    {
      if (thread != std::this_thread::get_id()) std::terminate();
    }
    std::array<Atom, 4> atoms() const { return {motif, opacity, normal, state}; }
    bool attributes(XWindowAttributes &a) const { return XGetWindowAttributes(display, xid, &a) != 0 && a.root == root; }
    /// @brief Preserve exact server properties and accepted asynchronous target before mutation.
    std::expected<void, core::AppError> snapshot()
    {
      Trap trap(display);
      XWindowAttributes a{};
      if (!attributes(a)) return std::unexpected(core::AppError{core::AppErrorCode::RuntimeStateLost});
      const auto names = atoms();
      Snapshot candidate;
      candidate.mapped = a.map_state != IsUnmapped;
      candidate.empty = input_empty;
      candidate.above = above_target;
      for (std::size_t i = 0; i < names.size(); ++i) {
        auto p = Property::read(display, xid, names[i]);
        if (!p) return std::unexpected(operation_error());
        candidate.properties[i] = std::move(*p);
      }
      if (!trap.finish()) return std::unexpected(core::AppError{trap.error == BadWindow ? core::AppErrorCode::RuntimeStateLost : core::AppErrorCode::PlatformOperationFailure, trap.error});
      saved = std::move(candidate);
      return {};
    }
    bool wm_above(bool target, bool mapped)
    {
      if (!caps.always_on_top) return !target;
      if (mapped) {
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = xid;
        event.xclient.message_type = state;
        event.xclient.format = 32;
        event.xclient.data.l[0] = target ? 1 : 0;
        event.xclient.data.l[1] = static_cast<long>(above);
        event.xclient.data.l[3] = 1;
        return XSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event) != 0;
      }
      auto p = Property::read(display, xid, state);
      if (!p) return false;
      if (p->type != None && (p->type != XA_ATOM || p->format != 32)) return false;
      auto values = p->type == None ? std::vector<unsigned long>{} : p->words();
      std::erase(values, above);
      if (target) values.push_back(above);
      XChangeProperty(display, xid, state, XA_ATOM, 32, PropModeReplace, reinterpret_cast<unsigned char *>(values.data()), static_cast<int>(values.size()));
      return true;
    }
    bool shape(bool empty)
    {
      if (!caps.pass_through) return !empty;
      if (empty)
        XShapeCombineRectangles(display, xid, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
      else
        XShapeCombineMask(display, xid, ShapeInput, 0, 0, None, ShapeSet);
      int count{}, ordering{};
      auto *rectangles = XShapeGetRectangles(display, xid, ShapeInput, &count, &ordering);
      bool valid = empty ? count == 0 : false;
      if (!empty) {
        XWindowAttributes a{};
        valid = attributes(a) && count == 1 && rectangles && rectangles[0].x == 0 && rectangles[0].y == 0 && rectangles[0].width == a.width && rectangles[0].height == a.height;
      }
      if (rectangles) XFree(rectangles);
      return valid;
    }
    core::Extent aspect(const core::ReplicaSettings &target) const
    {
      auto s = selection.snapshot();
      if (!s) return {};
      if (!target.selecting_region && target.region) {
        auto region = core::intersect_region(*target.region, s->content);
        if (region) return {region->width, region->height};
      }
      return s->content;
    }
    /// @brief Verify owned properties and Shape while accepting WM requests without waiting.
    /// Any failure leaves compensation to the surrounding complete settings transaction.
    std::expected<void, core::AppError> mutate(const core::ReplicaSettings &target)
    {
      if (target.mode == core::OverlayMode::PassThrough && !caps.pass_through) return std::unexpected(core::AppError{core::AppErrorCode::InvalidValue});
      const bool empty = !target.selecting_region && target.mode == core::OverlayMode::PassThrough;
      const int percent = target.selecting_region || !caps.window_opacity ? 100 : target.opacity_percent;
      Trap trap(display);
      XWindowAttributes a{};
      if (!attributes(a)) return std::unexpected(operation_error());
      if (caps.window_opacity) {
        const unsigned long value = static_cast<unsigned long>(static_cast<double>(percent) / 100.0 * 4294967295.0);
        XChangeProperty(display, xid, opacity, XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<const unsigned char *>(&value), 1);
      }
      // Preserve unrelated Motif words and normal-hint flags; only owned fields change.
      auto hints = saved.properties[0].type == None ? std::vector<unsigned long>(5) : saved.properties[0].words();
      if (saved.properties[0].type != None && (saved.properties[0].type != motif || saved.properties[0].format != 32 || hints.size() < 5)) return std::unexpected(operation_error());
      hints[0] |= 2;
      hints[2] = target.border == core::BorderPreference::Bordered ? 1 : 0;
      XChangeProperty(display, xid, motif, motif, 32, PropModeReplace, reinterpret_cast<unsigned char *>(hints.data()), static_cast<int>(hints.size()));
      auto extent = aspect(target);
      XSizeHints normal_hints{};
      long supplied{};
      XGetWMNormalHints(display, xid, &normal_hints, &supplied);
      if (extent.width && extent.height && extent.width <= INT_MAX && extent.height <= INT_MAX) {
        normal_hints.flags |= PAspect;
        normal_hints.min_aspect = {static_cast<int>(extent.width), static_cast<int>(extent.height)};
        normal_hints.max_aspect = normal_hints.min_aspect;
      }
      else
        normal_hints.flags &= ~PAspect;
      XSetWMNormalHints(display, xid, &normal_hints);
      const auto expected_normal = Property::read(display, xid, normal);
      const bool requested = wm_above(caps.always_on_top, a.map_state != IsUnmapped);
      const bool shaped = shape(empty);
      auto actual_motif = Property::read(display, xid, motif);
      auto actual_opacity = Property::read(display, xid, opacity);
      auto actual_normal = Property::read(display, xid, normal);
      bool verified = actual_motif && actual_motif->type == motif && actual_motif->format == 32 && actual_motif->words() == hints && expected_normal && actual_normal && *expected_normal == *actual_normal;
      if (caps.window_opacity) verified = verified && actual_opacity && actual_opacity->one(XA_CARDINAL, static_cast<unsigned long>(static_cast<double>(percent) / 100.0 * 4294967295.0));
      const bool native_ok = trap.finish();
      if (!requested || !shaped || !verified || !native_ok) return std::unexpected(operation_error(trap.error));
      input_empty = empty;
      above_target = caps.always_on_top;
      return {};
    }
    /// @brief Restore property absence/data, dynamic default Shape and previous mapped WM target.
    /// A delayed WM observation cannot reject compensation; rejected protocol work can.
    bool restore()
    {
      Trap trap(display);
      const auto names = atoms();
      XWindowAttributes a{};
      bool ok = attributes(a);
      for (std::size_t i = 0; i < names.size(); ++i)
        if (i != 3 || !saved.mapped) saved.properties[i].write(display, xid, names[i]);
      const bool compensated = !saved.mapped || wm_above(saved.above, true);
      const bool shaped = shape(saved.empty);
      for (std::size_t i = 0; i < names.size(); ++i)
        if (i != 3 || !saved.mapped) {
          auto p = Property::read(display, xid, names[i]);
          ok = ok && p && *p == saved.properties[i];
        }
      const bool native_ok = trap.finish();
      if (ok && compensated && shaped && native_ok) {
        input_empty = saved.empty;
        above_target = saved.above;
        return true;
      }
      return false;
    }
    std::expected<void, core::AppError> transaction(const core::ReplicaSettings &target, bool commit = true) noexcept
    {
      auto accepted = settings;
      auto r = detail::transact(accepted, target, {this, [](void *p) { return static_cast<Impl *>(p)->snapshot(); }, [](void *p, const core::ReplicaSettings &s) { return static_cast<Impl *>(p)->mutate(s); }, [](void *p) { return static_cast<Impl *>(p)->restore(); }});
      if (r && commit) settings = accepted;
      return r;
    }
    /// @brief Restore accepted preferences before exposing completion/cancellation to runtime.
    /// Failed restoration is terminal state loss even if its own transaction rolls back.
    void finish_selection() noexcept
    {
      if (!selection.terminal() || !selection_override) return;
      auto restored = settings;
      restored.selecting_region = false;
      auto r = transaction(restored, false);
      selection_override = false;
      if (!r)
        record_failure(core::AppError{core::AppErrorCode::RuntimeStateLost, r.error().native_detail});
      else {
        events.region_finished = true;
        events.region = selection.result();
      }
      scroll_remainder = 0;
    }
    void cancel() noexcept
    {
      selection.cancel();
      finish_selection();
    }
    /// @brief Keep accepted-state loss terminal until runtime takes the bounded facts.
    /// Later callbacks in the same pending batch cannot downgrade failed preference
    /// restoration merely because their own temporary-state rollback succeeded.
    void record_failure(core::AppError error) noexcept
    {
      if (!events.failure || events.failure->code != core::AppErrorCode::RuntimeStateLost)
        events.failure = error;
    }
    void fail(core::AppError e) noexcept
    {
      record_failure(e);
      cancel();
    }
    /// @brief Advertise only typed WM/compositor/Shape prerequisites verified on the owned window.
    bool probe()
    {
      caps.window_opacity = false;
      caps.source_selection_method = core::SourceSelectionMethod::SystemPicker;
      // Probe only prerequisites and property acceptance, never visual compositor behavior.
      const bool interactive = detail::interactive_supported(window);
      Trap trap(display);
      XWindowAttributes a{};
      if (!XGetWindowAttributes(display, xid, &a)) return false;
      root = a.root;
      auto supported = Property::read(display, root, XInternAtom(display, "_NET_SUPPORTED", False));
      if (interactive && supported && supported->type == XA_ATOM && supported->format == 32) {
        auto words = supported->words();
        caps.always_on_top = std::ranges::find(words, above) != words.end();
        caps.borderless_resize = true;
      }
      int event{}, error{}, major{}, minor{};
      caps.pass_through = XShapeQueryExtension(display, &event, &error) && XShapeQueryVersion(display, &major, &minor) && (major > 1 || (major == 1 && minor >= 1));
      char compositor[64]{};
      std::snprintf(compositor, sizeof(compositor), "_NET_WM_CM_S%d", XScreenNumberOfScreen(a.screen));
      const bool composed = XGetSelectionOwner(display, XInternAtom(display, compositor, False)) != None;
      auto initial_state = Property::read(display, xid, state);
      if (!initial_state) return false;
      if (initial_state->type != None) {
        if (initial_state->type != XA_ATOM || initial_state->format != 32) return false;
        auto words = initial_state->words();
        above_target = std::ranges::find(words, above) != words.end();
      }
      auto before = Property::read(display, xid, opacity);
      if (!before) return false;
      bool roundtrip = false;
      if (composed) {
        // Prepare compensation storage before changing the borrowed window. The
        // guard restores on allocation failure without allocating inside unwind.
        struct RestoreOpacity {
          Display *display;
          Window window;
          Atom atom;
          const Property &before;
          std::vector<unsigned long> words;
          bool active{true};
          void restore() const noexcept
          {
            if (before.type == None)
              XDeleteProperty(display, window, atom);
            else
              XChangeProperty(display, window, atom, before.type, before.format, PropModeReplace,
                              before.format == 32 ? reinterpret_cast<const unsigned char *>(words.data()) : before.bytes.data(),
                              static_cast<int>(before.format == 32 ? words.size() : before.bytes.size() / (before.format / 8)));
          }
          ~RestoreOpacity()
          {
            if (active) restore();
          }
        } compensation{display, xid, opacity, *before, before->format == 32 ? before->words() : std::vector<unsigned long>{}};
        probe_state_lost = true;
        const unsigned long word = 0xffffffffUL;
        XChangeProperty(display, xid, opacity, XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<const unsigned char *>(&word), 1);
        auto p = Property::read(display, xid, opacity);
        roundtrip = p && p->one(XA_CARDINAL, word);
        compensation.restore();
        auto restored = Property::read(display, xid, opacity);
        if (!restored || *restored != *before) return false;
        compensation.active = false;
        if (!trap.finish()) return false;
        probe_state_lost = false;
      }
      caps.window_opacity = composed && roundtrip;
      return trap.finish();
    }
    /// @brief Read current owned client geometry and actual WM frame insets, independent of GLFW caches.
    /// Missing frame extents mean zero insets; malformed typed data rejects the
    /// geometry. This never requests WM completion or consumes queued events.
    std::expected<std::pair<detail::ClientRect, detail::Insets>, core::AppError> current_geometry()
    {
      Trap trap(display);
      XWindowAttributes a{};
      Window child{};
      int x{}, y{};
      if (!attributes(a) || !XTranslateCoordinates(display, xid, root, 0, 0, &x, &y, &child))
        return std::unexpected(core::AppError{core::AppErrorCode::RuntimeStateLost});
      auto frame = Property::read(display, xid, XInternAtom(display, "_NET_FRAME_EXTENTS", False));
      if (!frame) return std::unexpected(operation_error());
      detail::Insets insets;
      if (frame->type != None) {
        if (frame->type != XA_CARDINAL || frame->format != 32 || frame->bytes.size() != 16)
          return std::unexpected(operation_error());
        const auto words = frame->words();
        for (auto word : words)
          if (word > static_cast<unsigned long>(std::numeric_limits<int>::max()))
            return std::unexpected(operation_error());
        insets = {static_cast<int>(words[0]), static_cast<int>(words[2]), static_cast<int>(words[1]), static_cast<int>(words[3])};
      }
      if (!trap.finish()) return std::unexpected(core::AppError{trap.error == BadWindow ? core::AppErrorCode::RuntimeStateLost : core::AppErrorCode::PlatformOperationFailure, trap.error});
      return std::pair{detail::ClientRect{x, y, a.width, a.height}, insets};
    }
    /// @brief Request session-only centered zoom from retired cropped aspect and current work area.
    /// Fractional input remains bounded; selection and preference changes clear the remainder.
    void wheel(double amount)
    {
      if (settings.selecting_region || selection.active() || settings.mode != core::OverlayMode::Editable || !std::isfinite(amount)) return;
      scroll_remainder = std::clamp(scroll_remainder + amount, -100.0, 100.0);
      const int notches = static_cast<int>(scroll_remainder);
      if (!notches) return;
      scroll_remainder -= notches;
      const auto geometry = current_geometry();
      if (!geometry) {
        fail(geometry.error());
        return;
      }
      const auto &client = geometry->first;
      int count{};
      auto **monitors = glfwGetMonitors(&count);
      double closest = std::numeric_limits<double>::max();
      std::optional<detail::ClientRect> work;
      const double center_x = client.x + client.width / 2.0, center_y = client.y + client.height / 2.0;
      for (int i = 0; i < count; ++i) {
        int x{}, y{}, width{}, height{};
        glfwGetMonitorWorkarea(monitors[i], &x, &y, &width, &height);
        if (width <= 0 || height <= 0) continue;
        const double distance = std::hypot(center_x - std::clamp(center_x, static_cast<double>(x), static_cast<double>(x) + width),
                                           center_y - std::clamp(center_y, static_cast<double>(y), static_cast<double>(y) + height));
        if (distance < closest) {
          closest = distance;
          work = detail::ClientRect{x, y, width, height};
        }
      }
      if (!work) return;
      auto proposal = detail::wheel_geometry(client, aspect(settings), *work, geometry->second, scale(window), notches, settings.wheel_zoom_percent);
      if (!proposal) return;
      // ConfigureRequest is accepted asynchronously. Do not demand immediate geometry.
      Trap trap(display);
      XMoveResizeWindow(display, xid, proposal->x, proposal->y, static_cast<unsigned>(proposal->width), static_cast<unsigned>(proposal->height));
      if (!trap.finish()) fail(operation_error(trap.error));
    }
    /// @brief Contain every exception at the C boundary and coalesce native failure facts.
    template <class F>
    static void dispatch(GLFWwindow *w, F f) noexcept
    {
      auto it = owners.find(w);
      if (it == owners.end()) return;
      auto *self = it->second;
      try {
        f(*self);
      } catch (...) {
        self->fail(operation_error());
      }
    }
    static void button(GLFWwindow *w, int button, int action, int mods) noexcept
    {
      dispatch(w, [&](Impl &s) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
          double x{}, y{};
          glfwGetCursorPos(w, &x, &y);
          if (s.selection.active()) {
            if (action == GLFW_PRESS)
              s.selection.press(x, y);
            else if (action == GLFW_RELEASE)
              s.selection.release(x, y);
            s.finish_selection();
          }
          else if (!s.settings.selecting_region && s.settings.mode == core::OverlayMode::Editable && s.settings.border == core::BorderPreference::Borderless && s.caps.borderless_resize) {
            if (action == GLFW_PRESS) {
              auto direction = detail::hit_direction(x, y, client_extent(w), scale(w), 1, 1);
              if (direction >= 0) {
                auto r = detail::moveresize(w, direction, x, y);
                s.gesturing = r.has_value();
                if (!r) s.fail(r.error());
              }
            }
            else if (action == GLFW_RELEASE && s.gesturing) {
              auto r = detail::moveresize(w, 11, x, y);
              s.gesturing = false;
              if (!r) s.fail(r.error());
            }
          }
        }
        try {
          if (s.previous_button) s.previous_button(w, button, action, mods);
        } catch (...) {
        }
      });
    }
    static void cursor(GLFWwindow *w, double x, double y) noexcept
    {
      dispatch(w, [&](Impl &s) {s.selection.move(x,y);try{if(s.previous_cursor)s.previous_cursor(w,x,y);}catch(...){}; });
    }
    static void scroll(GLFWwindow *w, double x, double y) noexcept
    {
      dispatch(w, [&](Impl &s) {s.wheel(y);try{if(s.previous_scroll)s.previous_scroll(w,x,y);}catch(...){}; });
    }
    static void key(GLFWwindow *w, int key, int scancode, int action, int mods) noexcept
    {
      dispatch(w, [&](Impl &s) {if(key==GLFW_KEY_ESCAPE&&action==GLFW_PRESS)s.cancel();try{if(s.previous_key)s.previous_key(w,key,scancode,action,mods);}catch(...){}; });
    }
    static void focus(GLFWwindow *w, int focused) noexcept
    {
      dispatch(w, [&](Impl &s) {if(!focused){s.cancel();s.gesturing=false;}try{if(s.previous_focus)s.previous_focus(w,focused);}catch(...){}; });
    }
    static void size(GLFWwindow *w, int width, int height) noexcept
    {
      dispatch(w, [&](Impl &s) {s.cancel();int fw{},fh{};glfwGetFramebufferSize(w,&fw,&fh);s.events.resized=core::Extent{static_cast<unsigned>(std::max(fw,0)),static_cast<unsigned>(std::max(fh,0))};try{if(s.previous_size)s.previous_size(w,width,height);}catch(...){}; });
    }
    static void framebuffer(GLFWwindow *w, int width, int height) noexcept
    {
      dispatch(w, [&](Impl &s) {s.cancel();s.events.resized=core::Extent{static_cast<unsigned>(std::max(width,0)),static_cast<unsigned>(std::max(height,0))};try{if(s.previous_framebuffer)s.previous_framebuffer(w,width,height);}catch(...){}; });
    }
    static void close(GLFWwindow *w) noexcept
    {
      dispatch(w, [&](Impl &s) {s.cancel();s.events.close_requested=true;try{if(s.previous_close)s.previous_close(w);}catch(...){}; });
    }
    ~Impl()
    {
      if (!attached) return;
      check_thread();
      owners.erase(window);
      glfwSetMouseButtonCallback(window, previous_button);
      glfwSetCursorPosCallback(window, previous_cursor);
      glfwSetScrollCallback(window, previous_scroll);
      glfwSetKeyCallback(window, previous_key);
      glfwSetWindowFocusCallback(window, previous_focus);
      glfwSetWindowSizeCallback(window, previous_size);
      glfwSetFramebufferSizeCallback(window, previous_framebuffer);
      glfwSetWindowCloseCallback(window, previous_close);
    }
  };
  X11OverlayAdapter::X11OverlayAdapter(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
  X11OverlayAdapter::~X11OverlayAdapter() = default;
  std::expected<std::unique_ptr<X11OverlayAdapter>, core::AppError> X11OverlayAdapter::create(GLFWwindow *w)
  {
    if (!w || glfwGetPlatform() != GLFW_PLATFORM_X11 || Impl::owners.contains(w)) return std::unexpected(core::AppError{core::AppErrorCode::RuntimeConstructionFailure});
    auto p = std::make_unique<Impl>();
    p->display = glfwGetX11Display();
    p->xid = glfwGetX11Window(w);
    if (!p->display || !p->xid) return std::unexpected(core::AppError{core::AppErrorCode::RuntimeConstructionFailure});
    p->motif = XInternAtom(p->display, "_MOTIF_WM_HINTS", False);
    p->opacity = XInternAtom(p->display, "_NET_WM_WINDOW_OPACITY", False);
    p->state = XInternAtom(p->display, "_NET_WM_STATE", False);
    p->above = XInternAtom(p->display, "_NET_WM_STATE_ABOVE", False);
    p->normal = XInternAtom(p->display, "WM_NORMAL_HINTS", False);
    // Probe borrows the GLFW window but does not install callback reachability yet.
    p->window = w;
    try {
      if (!p->probe()) {
        return std::unexpected(core::AppError{p->probe_state_lost ? core::AppErrorCode::RuntimeStateLost : core::AppErrorCode::RuntimeConstructionFailure});
      }
    } catch (...) {
      return std::unexpected(core::AppError{p->probe_state_lost ? core::AppErrorCode::RuntimeStateLost : core::AppErrorCode::RuntimeConstructionFailure});
    }
    p->window = nullptr;
    auto owner = std::unique_ptr<X11OverlayAdapter>(new X11OverlayAdapter(std::move(p)));
    Impl::owners.emplace(w, owner->impl_.get());
    auto &s = *owner->impl_;
    s.window = w;
    s.attached = true;
    s.previous_button = glfwSetMouseButtonCallback(w, Impl::button);
    s.previous_cursor = glfwSetCursorPosCallback(w, Impl::cursor);
    s.previous_scroll = glfwSetScrollCallback(w, Impl::scroll);
    s.previous_key = glfwSetKeyCallback(w, Impl::key);
    s.previous_focus = glfwSetWindowFocusCallback(w, Impl::focus);
    s.previous_size = glfwSetWindowSizeCallback(w, Impl::size);
    s.previous_framebuffer = glfwSetFramebufferSizeCallback(w, Impl::framebuffer);
    s.previous_close = glfwSetWindowCloseCallback(w, Impl::close);
    return owner;
  }
  std::expected<void, core::AppError> X11OverlayAdapter::apply(const core::ReplicaSettings &candidate) noexcept
  {
    auto &s = *impl_;
    s.check_thread();
    const auto mapping = s.selection.snapshot();
    if (candidate.region && mapping && !core::valid_region(*candidate.region, mapping->content)) return std::unexpected(core::AppError{core::AppErrorCode::InvalidValue});
    const auto old = s.settings;
    auto r = s.transaction(candidate);
    if (!r) return r;
    if (old.mode != candidate.mode || old.wheel_zoom_percent != candidate.wheel_zoom_percent || old.selecting_region != candidate.selecting_region) s.scroll_remainder = 0;
    if (candidate.selecting_region && !old.selecting_region) {
      s.selection.begin();
      s.selection_override = true;
    }
    else if (!candidate.selecting_region && old.selecting_region) {
      s.selection.cancel();
      s.selection_override = false;
    }
    return {};
  }
  void X11OverlayAdapter::set_presentation(render::PresentationSnapshot snapshot) noexcept
  {
    auto &s = *impl_;
    s.check_thread();
    const auto previous = s.selection.snapshot();
    s.selection.publish(snapshot, client_extent(s.window));
    s.finish_selection();
    // Failed accepted-style restoration requires drain. Do not continue mutating
    // temporary selection state in this update or a later pending presentation.
    if (s.events.failure && s.events.failure->code == core::AppErrorCode::RuntimeStateLost) return;
    // Retired content, rather than capture allocation, authorizes new native aspect hints.
    if (snapshot.content.width && snapshot.content.height && snapshot.framebuffer.width && snapshot.framebuffer.height &&
        (!previous || previous->content != snapshot.content) &&
        s.selection.snapshot() && s.selection.snapshot()->content == snapshot.content &&
        s.selection.snapshot()->present_sequence == snapshot.present_sequence) {
      auto target = s.settings;
      target.selecting_region = s.selection_override;
      auto r = s.transaction(target, false);
      if (!r) s.fail(r.error());
    }
  }
  X11OverlayEvents X11OverlayAdapter::take_events() noexcept
  {
    impl_->check_thread();
    auto events = impl_->events;
    impl_->events = {};
    return events;
  }
  core::PlatformCapabilities X11OverlayAdapter::capabilities() const noexcept
  {
    impl_->check_thread();
    return impl_->caps;
  }
  std::optional<core::PixelRegion> X11OverlayAdapter::selection_preview() const noexcept
  {
    impl_->check_thread();
    return impl_->selection.preview();
  }
}    // namespace avb::linux_platform
