#include "platform/windows/overlay_window.hpp"

#include <GLFW/glfw3.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace airelinna::platform::windows {
namespace {

[[nodiscard]] constexpr core::AppError overlay_error(
    const core::AppErrorCode code,
    const std::optional<std::int64_t> native_detail = std::nullopt) noexcept {
  return {.code = code, .native_detail = native_detail};
}

[[nodiscard]] std::expected<core::BorderPreference, core::AppError>
observed_border(GLFWwindow *const window) noexcept {
  static_cast<void>(glfwGetError(nullptr));
  const auto decorated = glfwGetWindowAttrib(window, GLFW_DECORATED);
  const auto native_detail = glfwGetError(nullptr);
  if (native_detail != GLFW_NO_ERROR) {
    return std::unexpected{overlay_error(
        core::AppErrorCode::PlatformOperationFailure, native_detail)};
  }
  return decorated == GLFW_TRUE ? core::BorderPreference::Bordered
                                : core::BorderPreference::Borderless;
}

} // namespace

struct OverlayWindow::Impl final {
  Impl(GLFWwindow *owned_window,
       const core::BorderPreference initial_border) noexcept
      : window{owned_window}, border{initial_border} {}

  ~Impl() noexcept {
    if (window != nullptr) {
      glfwDestroyWindow(window);
    }
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;

  GLFWwindow *window{};
  core::BorderPreference border{core::BorderPreference::Bordered};
};

std::expected<std::unique_ptr<OverlayWindow>, core::AppError>
OverlayWindow::create(const core::Extent initial_extent,
                      const core::BorderPreference border) {
  constexpr auto max_glfw_dimension =
      static_cast<std::uint32_t>((std::numeric_limits<int>::max)());
  if (initial_extent.width == 0 || initial_extent.height == 0 ||
      initial_extent.width > max_glfw_dimension ||
      initial_extent.height > max_glfw_dimension) {
    return std::unexpected{overlay_error(core::AppErrorCode::InvalidValue)};
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
  glfwWindowHint(GLFW_DECORATED, border == core::BorderPreference::Bordered
                                     ? GLFW_TRUE
                                     : GLFW_FALSE);
  auto *const window = glfwCreateWindow(static_cast<int>(initial_extent.width),
                                        static_cast<int>(initial_extent.height),
                                        "Airelinna Replica", nullptr, nullptr);
  if (window == nullptr) {
    const char *description{};
    const auto native_detail = glfwGetError(&description);
    static_cast<void>(description);
    return std::unexpected{overlay_error(
        core::AppErrorCode::RuntimeConstructionFailure, native_detail)};
  }

  const auto destroy_glfw_window = [](GLFWwindow *owned_window) noexcept {
    if (owned_window != nullptr) {
      glfwDestroyWindow(owned_window);
    }
  };
  std::unique_ptr<GLFWwindow, decltype(destroy_glfw_window)> window_guard{
      window, destroy_glfw_window};
  auto implementation = std::make_unique<Impl>(window_guard.get(), border);
  static_cast<void>(window_guard.release());
  return std::make_unique<OverlayWindow>(ConstructionToken{},
                                         std::move(implementation));
}

OverlayWindow::OverlayWindow(ConstructionToken,
                             std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)} {}

OverlayWindow::~OverlayWindow() noexcept { destroy(); }

std::expected<void, core::AppError>
OverlayWindow::set_border(const core::BorderPreference border) noexcept {
  if (!implementation_ || implementation_->window == nullptr) {
    return std::unexpected{
        overlay_error(core::AppErrorCode::PlatformOperationFailure)};
  }
  const auto current = observed_border(implementation_->window);
  if (!current) {
    return std::unexpected{current.error()};
  }
  if (*current == border) {
    implementation_->border = border;
    return {};
  }

  static_cast<void>(glfwGetError(nullptr));
  glfwSetWindowAttrib(implementation_->window, GLFW_DECORATED,
                      border == core::BorderPreference::Bordered ? GLFW_TRUE
                                                                 : GLFW_FALSE);
  const auto native_detail = glfwGetError(nullptr);
  if (native_detail != GLFW_NO_ERROR) {
    return std::unexpected{overlay_error(
        core::AppErrorCode::PlatformOperationFailure, native_detail)};
  }
  const auto verified = observed_border(implementation_->window);
  if (!verified) {
    return std::unexpected{verified.error()};
  }
  if (*verified != border) {
    return std::unexpected{
        overlay_error(core::AppErrorCode::PlatformOperationFailure)};
  }
  implementation_->border = border;
  return {};
}

core::BorderPreference OverlayWindow::border() const noexcept {
  return implementation_ ? implementation_->border
                         : core::BorderPreference::Bordered;
}

bool OverlayWindow::border_matches(
    const core::BorderPreference border) const noexcept {
  if (!implementation_ || implementation_->window == nullptr) {
    return false;
  }
  const auto observed = observed_border(implementation_->window);
  return observed && *observed == border;
}

bool OverlayWindow::close_requested() const noexcept {
  return implementation_ && implementation_->window != nullptr &&
         glfwWindowShouldClose(implementation_->window) == GLFW_TRUE;
}

void OverlayWindow::set_close_requested(const bool requested) noexcept {
  if (implementation_ && implementation_->window != nullptr) {
    glfwSetWindowShouldClose(implementation_->window,
                             requested ? GLFW_TRUE : GLFW_FALSE);
  }
}

core::Extent OverlayWindow::framebuffer_extent() const noexcept {
  if (!implementation_ || implementation_->window == nullptr) {
    return {};
  }
  int width{};
  int height{};
  glfwGetFramebufferSize(implementation_->window, &width, &height);
  if (width <= 0 || height <= 0) {
    return {};
  }
  return {.width = static_cast<std::uint32_t>(width),
          .height = static_cast<std::uint32_t>(height)};
}

void OverlayWindow::destroy() noexcept {
  if (implementation_ && implementation_->window != nullptr) {
    glfwDestroyWindow(implementation_->window);
    implementation_->window = nullptr;
  }
}

GLFWwindow *OverlayWindow::native_window() const noexcept {
  return implementation_ ? implementation_->window : nullptr;
}

} // namespace airelinna::platform::windows
