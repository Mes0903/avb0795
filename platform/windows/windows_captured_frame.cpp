/// @file
/// @brief Move-only WGC frame checkout ownership and borrowed D3D texture access.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/windows_captured_frame.hpp"

#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>

#include <utility>

namespace airelinna::platform::windows {

  WindowsCapturedFrame::WindowsCapturedFrame(
    const std::uint64_t sequence, const std::uint64_t resource_generation, const core::Extent allocation_extent, const core::Extent content_size, const winrt::Windows::Graphics::DirectX::DirectXPixelFormat expected_pool_format, const std::chrono::steady_clock::time_point capture_timestamp, winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame)
      : sequence_(sequence), resource_generation_(resource_generation), allocation_extent_(allocation_extent), content_size_(content_size), expected_pool_format_(expected_pool_format), capture_timestamp_(capture_timestamp), frame_(std::move(frame))
  {
    if (!frame_) {
      throw winrt::hresult_invalid_argument{};
    }
  }

  WindowsCapturedFrame::~WindowsCapturedFrame() noexcept { close_checkout(); }

  WindowsCapturedFrame::WindowsCapturedFrame(
    WindowsCapturedFrame &&other) noexcept
      : sequence_(other.sequence_),
        resource_generation_(other.resource_generation_),
        allocation_extent_(other.allocation_extent_),
        content_size_(other.content_size_),
        expected_pool_format_(other.expected_pool_format_),
        capture_timestamp_(other.capture_timestamp_),
        frame_(std::move(other.frame_)) {}

  WindowsCapturedFrame &
  WindowsCapturedFrame::operator=(WindowsCapturedFrame &&other) noexcept
  {
    if (this == &other) {
      return *this;
    }

    // Return the previous checkout before adopting a different pool buffer.
    close_checkout();
    sequence_ = other.sequence_;
    resource_generation_ = other.resource_generation_;
    allocation_extent_ = other.allocation_extent_;
    content_size_ = other.content_size_;
    expected_pool_format_ = other.expected_pool_format_;
    capture_timestamp_ = other.capture_timestamp_;
    frame_ = std::move(other.frame_);
    return *this;
  }

  std::uint64_t WindowsCapturedFrame::sequence() const noexcept
  {
    return sequence_;
  }

  std::uint64_t WindowsCapturedFrame::resource_generation() const noexcept
  {
    return resource_generation_;
  }

  core::Extent WindowsCapturedFrame::allocation_extent() const noexcept
  {
    return allocation_extent_;
  }

  core::Extent WindowsCapturedFrame::content_size() const noexcept
  {
    return content_size_;
  }

  winrt::Windows::Graphics::DirectX::DirectXPixelFormat
  WindowsCapturedFrame::expected_pool_format() const noexcept
  {
    return expected_pool_format_;
  }

  std::chrono::steady_clock::time_point
  WindowsCapturedFrame::capture_timestamp() const noexcept
  {
    return capture_timestamp_;
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> WindowsCapturedFrame::texture() const
  {
    if (!frame_) {
      throw winrt::hresult_illegal_method_call{};
    }

    // A COM texture view is not permission to return the WGC checkout early.
    const auto surface_access = frame_.Surface()
                                  .as<::Windows::Graphics::DirectX::Direct3D11::
                                        IDirect3DDxgiInterfaceAccess>();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    winrt::check_hresult(
      surface_access->GetInterface(IID_PPV_ARGS(texture.GetAddressOf())));
    return texture;
  }

  void WindowsCapturedFrame::close_checkout() noexcept
  {
    // Clear ownership before Close so a moved/destructed wrapper cannot close it twice.
    auto frame = std::exchange(frame_, nullptr);
    if (!frame) {
      return;
    }

    try {
      frame.Close();
    } catch (...) {
      // The checkout is released below even when the projected Close reports an
      // error. Destruction must not allow an exception to cross the RAII path.
    }
  }

}    // namespace airelinna::platform::windows
