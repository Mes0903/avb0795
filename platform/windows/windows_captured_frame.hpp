/// @file
/// @brief Move-only WGC frame checkout ownership and borrowed D3D texture access.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_CAPTURED_FRAME_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_CAPTURED_FRAME_HPP

#include "avb/core/types.hpp"

#include <d3d11.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>

namespace avb::platform::windows {

  /// @brief Retain a WGC pool checkout until the last scheduled GPU use retires.
  ///
  /// Moving transfers responsibility for attempting Close exactly once. A COM
  /// reference to the texture cannot substitute for checkout ownership because
  /// returning the frame allows the pool to reuse its storage. The pool owner
  /// must outlive instances held by intake, the mailbox, and rendering work.
  /// Generation and allocation metadata travel with the checkout so consumers
  /// can reject work from an allocation that has already been superseded.
  class WindowsCapturedFrame final {
  public:
    /// Adopt a checked-out frame together with immutable generation metadata.
    WindowsCapturedFrame(
      std::uint64_t sequence, std::uint64_t resource_generation, core::Extent allocation_extent, core::Extent content_size, winrt::Windows::Graphics::DirectX::DirectXPixelFormat expected_pool_format, std::chrono::steady_clock::time_point capture_timestamp, winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame);

    /// Return any owned checkout without allowing WinRT exceptions to escape.
    ~WindowsCapturedFrame() noexcept;

    WindowsCapturedFrame(const WindowsCapturedFrame &) = delete;
    WindowsCapturedFrame &operator=(const WindowsCapturedFrame &) = delete;

    WindowsCapturedFrame(WindowsCapturedFrame &&other) noexcept;
    WindowsCapturedFrame &operator=(WindowsCapturedFrame &&other) noexcept;

    /// Intake sequence used to distinguish newer frames within capture work.
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    /// Runtime-assigned resource generation; old allocations must not be reused.
    [[nodiscard]] std::uint64_t resource_generation() const noexcept;
    /// Physical pool allocation, which may temporarily exceed valid content.
    [[nodiscard]] core::Extent allocation_extent() const noexcept;
    /// Valid ContentSize used to clip copy and sampling operations.
    [[nodiscard]] core::Extent content_size() const noexcept;
    /// Immutable pool format expected by this frame's capture generation.
    [[nodiscard]] winrt::Windows::Graphics::DirectX::DirectXPixelFormat
    expected_pool_format() const noexcept;
    /// Host steady-clock observation time recorded by intake.
    [[nodiscard]] std::chrono::steady_clock::time_point
    capture_timestamp() const noexcept;

    /// @brief Obtain a COM texture view without transferring the pool checkout.
    ///
    /// Keep this wrapper alive until every submitted use of the view completes.
    /// Projection failure throws; calling on a moved-from wrapper also throws.
    /// The returned COM pointer retains the texture interface, not permission to
    /// return the frame early or reuse it after its checkout has been closed.
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Texture2D> texture() const;

  private:
    void close_checkout() noexcept;

    std::uint64_t sequence_{};
    std::uint64_t resource_generation_{};
    core::Extent allocation_extent_{};
    core::Extent content_size_{};
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat expected_pool_format_{};
    std::chrono::steady_clock::time_point capture_timestamp_{};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame_{nullptr};
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_CAPTURED_FRAME_HPP
