/// @file
/// @brief EWMH interactive movement with host-independent callback lookup.
#include "platform/linux/x11_controller_window.hpp"
#include "platform/linux/detail/overlay_policy.hpp"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#include <unordered_map>
namespace avb::linux_platform {
  namespace {
    /// @brief Main-thread-only protocol attribution; earlier errors are flushed first.
    /// Exact display and serial bounds protect the predecessor from unrelated errors.
    /// Event dispatch/polling and GLFW operations installing their own error traps
    /// are forbidden until finish restores the handler. Only the inspected pinned
    /// non-nested size-limit setter used by minimum is permitted inside this scope.
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
    /// @brief Install the controller minimum through inspected GLFW native work and verify the server.
    /// The pinned setter has no nested error trap. Failure restores exact prior
    /// normal-hint protocol words; caller discards this uncommitted GLFW window.
    std::expected<void, core::AppError> minimum(GLFWwindow *window)
    {
      auto *d = glfwGetX11Display();
      const auto w = glfwGetX11Window(window);
      Trap trap(d);
      Atom actual{};
      int format{};
      unsigned long count{}, remaining{};
      unsigned char *data{};
      const auto status = XGetWindowProperty(d, w, XA_WM_NORMAL_HINTS, 0, 64, False, AnyPropertyType, &actual, &format, &count, &remaining, &data);
      struct Free {
        unsigned char *p;
        ~Free()
        {
          if (p) XFree(p);
        }
      } cleanup{data};
      if (status != Success || remaining || (actual != None && (actual != XA_WM_SIZE_HINTS || format != 32 || count > 64)))
        return std::unexpected(core::AppError{core::AppErrorCode::RuntimeConstructionFailure});
      glfwSetWindowSizeLimits(window, 320, 240, GLFW_DONT_CARE, GLFW_DONT_CARE);
      XSizeHints hints{};
      long supplied{};
      const bool checked = XGetWMNormalHints(d, w, &hints, &supplied) && (hints.flags & PMinSize) && hints.min_width == 320 && hints.min_height == 240;
      const bool ok = trap.finish();
      if (checked && ok) return {};
      Trap compensation(d);
      if (actual == None)
        XDeleteProperty(d, w, XA_WM_NORMAL_HINTS);
      else
        XChangeProperty(d, w, XA_WM_NORMAL_HINTS, actual, format, PropModeReplace, data, static_cast<int>(count));
      Atom restored_type{};
      int restored_format{};
      unsigned long restored_count{}, restored_remaining{};
      unsigned char *restored{};
      XGetWindowProperty(d, w, XA_WM_NORMAL_HINTS, 0, 64, False, AnyPropertyType, &restored_type, &restored_format, &restored_count, &restored_remaining, &restored);
      Free restored_cleanup{restored};
      bool matches = restored_type == actual && restored_format == format && restored_count == count && !restored_remaining;
      if (matches && format == 32)
        for (unsigned long i = 0; i < count; ++i)
          matches = matches && static_cast<std::uint32_t>(reinterpret_cast<unsigned long *>(data)[i]) == static_cast<std::uint32_t>(reinterpret_cast<unsigned long *>(restored)[i]);
      const bool safe = compensation.finish();
      return std::unexpected(core::AppError{matches && safe ? core::AppErrorCode::RuntimeConstructionFailure : core::AppErrorCode::RuntimeStateLost, trap.error});
    }
    bool atom_property(Display *d, Window w, Atom property, Atom type, Atom expected)
    {
      Atom actual{};
      int format{};
      unsigned long count{}, remaining{};
      unsigned char *data{};
      const int status = XGetWindowProperty(d, w, property, 0, 4096, False, type, &actual, &format, &count, &remaining, &data);
      bool found = false;
      if (status == Success && actual == type && format == 32 && remaining == 0 && (type != XA_WINDOW || count == 1))
        for (unsigned long i = 0; i < count; ++i)
          if ((reinterpret_cast<unsigned long *>(data)[i] & 0xffffffffUL) == expected) found = true;
      if (data) XFree(data);
      return found;
    }
  }    // namespace
  namespace detail {
    bool interactive_supported(GLFWwindow *window) noexcept
    {
      if (!window || glfwGetPlatform() != GLFW_PLATFORM_X11) return false;
      auto *d = glfwGetX11Display();
      const auto w = glfwGetX11Window(window);
      if (!d || !w) return false;
      Trap trap(d);
      XWindowAttributes a{};
      if (!XGetWindowAttributes(d, w, &a)) return false;
      const auto check = XInternAtom(d, "_NET_SUPPORTING_WM_CHECK", False);
      Atom actual{};
      int format{};
      unsigned long n{}, remaining{};
      unsigned char *data{};
      const int status = XGetWindowProperty(d, a.root, check, 0, 1, False, XA_WINDOW, &actual, &format, &n, &remaining, &data);
      Window wm = 0;
      if (status == Success && actual == XA_WINDOW && format == 32 && n == 1 && remaining == 0) wm = reinterpret_cast<unsigned long *>(data)[0] & 0xffffffffUL;
      if (data) XFree(data);
      const bool valid = wm && atom_property(d, wm, check, XA_WINDOW, wm) && atom_property(d, a.root, XInternAtom(d, "_NET_SUPPORTED", False), XA_ATOM, XInternAtom(d, "_NET_WM_MOVERESIZE", False));
      return trap.finish() && valid;
    }
    std::expected<void, core::AppError> moveresize(GLFWwindow *window, int direction, double x, double y) noexcept
    {
      if (!window || direction < 0 || direction > 11 || !std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 2147483647.0 || std::abs(y) > 2147483647.0) return std::unexpected(core::AppError{core::AppErrorCode::InvalidValue});
      auto *d = glfwGetX11Display();
      auto w = glfwGetX11Window(window);
      Trap trap(d);
      XWindowAttributes a{};
      Window child{};
      int rx{}, ry{};
      if (!XGetWindowAttributes(d, w, &a) || !XTranslateCoordinates(d, w, a.root, 0, 0, &rx, &ry, &child)) return std::unexpected(core::AppError{core::AppErrorCode::PlatformOperationFailure});
      // Translate the origin, then add the client point in widened arithmetic;
      // TranslateCoordinates itself has signed 16-bit source-coordinate fields.
      const auto root_x = static_cast<std::int64_t>(rx) + static_cast<int>(x);
      const auto root_y = static_cast<std::int64_t>(ry) + static_cast<int>(y);
      if (root_x < INT32_MIN || root_x > INT32_MAX || root_y < INT32_MIN || root_y > INT32_MAX)
        return std::unexpected(core::AppError{core::AppErrorCode::InvalidValue});
      // GLFW's client may hold a grab; WM gesture ownership requires releasing it first.
      XUngrabPointer(d, CurrentTime);
      XUngrabKeyboard(d, CurrentTime);
      XEvent event{};
      event.xclient.type = ClientMessage;
      event.xclient.window = w;
      event.xclient.message_type = XInternAtom(d, "_NET_WM_MOVERESIZE", False);
      event.xclient.format = 32;
      event.xclient.data.l[0] = static_cast<long>(root_x);
      event.xclient.data.l[1] = static_cast<long>(root_y);
      event.xclient.data.l[2] = direction;
      event.xclient.data.l[3] = 1;
      event.xclient.data.l[4] = 1;
      const auto sent = XSendEvent(d, a.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
      const bool ok = trap.finish();
      if (!sent || !ok) return std::unexpected(core::AppError{core::AppErrorCode::PlatformOperationFailure, trap.error});
      return {};
    }
  }    // namespace detail
  /// @brief Stable owner-thread receiver reached through the host/ImGui predecessor chain.
  /// It never replaces host user storage; detachment erases lookup before restoring
  /// the callback so a destroyed receiver cannot remain reachable by native input.
  struct X11ControllerWindow::Impl {
    GLFWwindow *window{};
    std::thread::id thread{std::this_thread::get_id()};
    float right{}, bottom{};
    GLFWmousebuttonfun previous{};
    bool gesturing{};
    // Lookup is owner-thread-only; insertion happens before callbacks become reachable.
    static inline std::unordered_map<GLFWwindow *, Impl *> owners;
    static void button(GLFWwindow *w, int button, int action, int mods) noexcept
    {
      auto it = owners.find(w);
      if (it == owners.end()) return;
      auto *self = it->second;
      try {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
          double x{}, y{};
          glfwGetCursorPos(w, &x, &y);
          if (action == GLFW_PRESS) {
            int width{}, height{};
            float sx{}, sy{};
            glfwGetWindowSize(w, &width, &height);
            glfwGetWindowContentScale(w, &sx, &sy);
            const int direction = detail::hit_direction(x, y, {static_cast<unsigned>(std::max(width, 0)), static_cast<unsigned>(std::max(height, 0))}, std::max(sx, sy), self->right, self->bottom);
            if (direction >= 0) self->gesturing = detail::moveresize(w, direction, x, y).has_value();
          }
          else if (action == GLFW_RELEASE && self->gesturing) {
            (void) detail::moveresize(w, 11, x, y);
            self->gesturing = false;
          }
        }
      } catch (...) {
      }
      try {
        if (self->previous) self->previous(w, button, action, mods);
      } catch (...) {
      }
    }
    ~Impl()
    {
      if (!window) return;
      if (thread != std::this_thread::get_id()) std::terminate();
      owners.erase(window);
      glfwSetMouseButtonCallback(window, previous);
    }
  };
  X11ControllerWindow::X11ControllerWindow(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
  X11ControllerWindow::~X11ControllerWindow() = default;
  std::expected<std::unique_ptr<desktop::ControllerWindowInteraction>, core::AppError> X11ControllerWindow::create(GLFWwindow *w)
  {
    if (!detail::interactive_supported(w) || Impl::owners.contains(w)) return std::unexpected(core::AppError{core::AppErrorCode::RuntimeConstructionFailure});
    auto p = std::make_unique<Impl>();
    if (auto limits = minimum(w); !limits) return std::unexpected(limits.error());
    auto owner = std::unique_ptr<X11ControllerWindow>(new X11ControllerWindow(std::move(p)));
    Impl::owners.emplace(w, owner->impl_.get());
    owner->impl_->window = w;
    owner->impl_->previous = glfwSetMouseButtonCallback(w, Impl::button);
    return std::unique_ptr<desktop::ControllerWindowInteraction>(std::move(owner));
  }
  void X11ControllerWindow::set_drag_region(float r, float b) noexcept
  {
    if (impl_->thread != std::this_thread::get_id()) std::terminate();
    impl_->right = std::isfinite(r) ? std::clamp(r, 0.f, 1.f) : 0;
    impl_->bottom = std::isfinite(b) ? std::clamp(b, 0.f, 1.f) : 0;
  }
  std::expected<std::string, core::AppError> portal_parent_identifier(GLFWwindow *w)
  {
    if (!w || glfwGetPlatform() != GLFW_PLATFORM_X11) return std::unexpected(core::AppError{core::AppErrorCode::RuntimeConstructionFailure});
    const auto xid = glfwGetX11Window(w);
    if (!xid) return std::unexpected(core::AppError{core::AppErrorCode::RuntimeConstructionFailure});
    char value[64]{};
    std::snprintf(value, sizeof(value), "x11:%lx", xid);
    return std::string(value);
  }
}    // namespace avb::linux_platform
