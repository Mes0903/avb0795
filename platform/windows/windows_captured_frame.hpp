#ifndef AIRELINNA_PLATFORM_WINDOWS_WINDOWS_CAPTURED_FRAME_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WINDOWS_CAPTURED_FRAME_HPP

#include "airelinna/core/types.hpp"

#include <d3d11.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>

namespace airelinna::platform::windows {

  class WindowsCapturedFrame final {
  public:
    WindowsCapturedFrame(
      std::uint64_t sequence, std::uint64_t resource_generation, core::Extent allocation_extent, core::Extent content_size, winrt::Windows::Graphics::DirectX::DirectXPixelFormat expected_pool_format, std::chrono::steady_clock::time_point capture_timestamp, winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame);

    ~WindowsCapturedFrame() noexcept;

    WindowsCapturedFrame(const WindowsCapturedFrame &) = delete;
    WindowsCapturedFrame &operator=(const WindowsCapturedFrame &) = delete;

    WindowsCapturedFrame(WindowsCapturedFrame &&other) noexcept;
    WindowsCapturedFrame &operator=(WindowsCapturedFrame &&other) noexcept;

    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] std::uint64_t resource_generation() const noexcept;
    [[nodiscard]] core::Extent allocation_extent() const noexcept;
    [[nodiscard]] core::Extent content_size() const noexcept;
    [[nodiscard]] winrt::Windows::Graphics::DirectX::DirectXPixelFormat
    expected_pool_format() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point
    capture_timestamp() const noexcept;

    // The returned texture is only a view. This wrapper must remain alive until
    // every use of the texture has completed so the WGC checkout stays owned.
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

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_CAPTURED_FRAME_HPP
