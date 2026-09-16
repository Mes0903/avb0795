/// @file
/// @brief D3 copy proof and checkout release, independent of renderer device lifetime.
#include "platform/windows/windows_frame_bridge.hpp"

#include "platform/windows/detail/windows_graphics_policy.hpp"
#include "platform/windows/windows_captured_frame.hpp"

#include <d3d11_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace avb::platform::windows {
  namespace {
    using Microsoft::WRL::ComPtr;

    constexpr std::size_t slot_count = 3;
    constexpr std::uint32_t bytes_per_pixel = 4;
    constexpr auto pending_poll_interval = std::chrono::milliseconds{2};

    [[nodiscard]] constexpr core::AppError bridge_error(
      const core::AppErrorCode code,
      const std::optional<std::int64_t> detail = std::nullopt) noexcept
    {
      return {.code = code, .native_detail = detail};
    }

    [[nodiscard]] constexpr core::AppError
    d3_error(const core::AppErrorCode code, const HRESULT result) noexcept
    {
      return bridge_error(code, static_cast<std::int64_t>(result));
    }

    // Validate the same allocation-sized byte count required by the CPU upload
    // owner before either side starts allocating or addressing source rows.
    [[nodiscard]] bool valid_allocation(const core::Extent extent) noexcept
    {
      if (extent.width == 0 || extent.height == 0) {
        return false;
      }
      constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
      const auto width = static_cast<std::uint64_t>(extent.width);
      const auto height = static_cast<std::uint64_t>(extent.height);
      if (width > maximum / height || width * height > maximum / bytes_per_pixel) {
        return false;
      }
      return width * height * bytes_per_pixel <=
             (std::numeric_limits<std::size_t>::max)();
    }

    /// @brief Unmap one successful native Map while its slot and context remain owned.
    class MappedReadback final {
    public:
      MappedReadback(ID3D11DeviceContext &context, ID3D11Texture2D &texture) noexcept
          : context_{context}, texture_{texture} {}
      ~MappedReadback() noexcept { context_.Unmap(&texture_, 0); }
      MappedReadback(const MappedReadback &) = delete;
      MappedReadback &operator=(const MappedReadback &) = delete;

    private:
      ID3D11DeviceContext &context_;
      ID3D11Texture2D &texture_;
    };
  }    // namespace

  class WindowsFrameBridge::Impl final {
  public:
    /// Each value names actual producer proof, never renderer readiness or use.
    enum class CopyProof { QueryPending, FencePending, CpuMapPending };

    struct PendingCopy final {
      std::optional<WindowsCapturedFrame> frame;
      core::Extent content{};
      render::FrameStamp stamp;
      CopyProof proof{CopyProof::QueryPending};
      // Submit borrows native interfaces briefly; these references keep copy
      // dependencies alive after that call and across independent Vulkan loss.
      ComPtr<ID3D11Texture2D> external_destination;
      ComPtr<ID3D11Fence> external_fence;
      std::uint64_t target{};
    };

    struct ProducerSlot final {
      render::SlotKey key;
      ComPtr<ID3D11Texture2D> staging;
      ComPtr<ID3D11Query> query;
      // Remains engaged after query proof releases frame when Map must retry.
      std::optional<PendingCopy> pending;
    };

    Impl()
    {
      constexpr std::array requested_levels{D3D_FEATURE_LEVEL_11_1,
                                            D3D_FEATURE_LEVEL_11_0};
      D3D_FEATURE_LEVEL selected_level{};
      const auto result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested_levels.data(),
        static_cast<UINT>(requested_levels.size()), D3D11_SDK_VERSION,
        device_.GetAddressOf(), &selected_level, context_.GetAddressOf());
      if (FAILED(result)) {
        throw d3_error(core::AppErrorCode::UnsupportedGraphics, result);
      }
      static_cast<void>(selected_level);
    }

    ~Impl() noexcept
    {
      // Even a latched removal is delivered as keyed events before destruction;
      // the caller must not abandon its canonical reservations during unwind.
      if (has_pending_inputs()) {
        std::terminate();
      }
    }

    [[nodiscard]] ID3D11Device &capture_device() noexcept { return *device_.Get(); }

    [[nodiscard]] std::expected<void, core::AppError>
    start_ring(const std::uint64_t serial, const std::uint64_t generation,
               const core::Extent allocation, const DXGI_FORMAT format) noexcept
    {
      if (serial <= last_issued_serial_ || generation == 0 || ring_serial_ != 0 ||
          !valid_allocation(allocation) || format != DXGI_FORMAT_B8G8R8A8_UNORM ||
          d3_device_removed_ || terminal_error_) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }
      ring_serial_ = serial;
      last_issued_serial_ = serial;
      generation_ = generation;
      allocation_ = allocation;
      format_ = format;
      for (std::size_t index{}; index < slots_.size(); ++index) {
        slots_[index].key = {serial, static_cast<std::uint32_t>(index)};
      }
      return {};
    }

    [[nodiscard]] std::expected<void, core::AppError>
    create_cpu_slot(const render::SlotKey key) noexcept
    {
      auto *const slot = find_slot(key);
      if (slot == nullptr || slot->staging || slot->query || slot->pending) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }
      D3D11_TEXTURE2D_DESC texture_info{};
      texture_info.Width = allocation_.width;
      texture_info.Height = allocation_.height;
      texture_info.MipLevels = 1;
      texture_info.ArraySize = 1;
      texture_info.Format = format_;
      texture_info.SampleDesc.Count = 1;
      texture_info.Usage = D3D11_USAGE_STAGING;
      texture_info.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      const auto result = device_->CreateTexture2D(
        &texture_info, nullptr, slot->staging.GetAddressOf());
      if (FAILED(result)) {
        return std::unexpected{
          d3_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
      }
      return create_query(*slot);
    }

    [[nodiscard]] std::expected<void, core::AppError>
    create_query_slot(const render::SlotKey key) noexcept
    {
      auto *const slot = find_slot(key);
      if (slot == nullptr || slot->staging || slot->query || slot->pending) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }
      return create_query(*slot);
    }

    [[nodiscard]] std::expected<void, core::AppError>
    release_ring(const std::uint64_t expected_serial) noexcept
    {
      if (expected_serial == 0 || expected_serial != ring_serial_ ||
          has_pending_inputs()) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }
      slots_ = {};
      ring_serial_ = 0;
      generation_ = 0;
      allocation_ = {};
      format_ = DXGI_FORMAT_UNKNOWN;
      return {};
    }

    [[nodiscard]] std::expected<BridgeSubmitResult, core::AppError>
    submit(const render::SlotKey key, WindowsCapturedFrame &&frame,
           const render::FrameStamp stamp, const ExternalCopyBorrow *const external)
    {
      auto *const slot = find_slot(key);
      const auto content = frame.content_size();
      if (slot == nullptr || slot->pending || !slot->query ||
          d3_device_removed_ || terminal_error_ ||
          frame.resource_generation() != generation_ ||
          frame.allocation_extent() != allocation_ ||
          frame.expected_pool_format() !=
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::
              B8G8R8A8UIntNormalized ||
          content.width == 0 || content.height == 0 ||
          content.width > allocation_.width || content.height > allocation_.height ||
          (external == nullptr && !slot->staging) ||
          (external != nullptr &&
           (slot->staging || external->destination == nullptr ||
            external->fence == nullptr || external->signal_context == nullptr ||
            external->target == 0))) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }

      // Projection can throw before adoption. A source texture reference alone
      // never substitutes for keeping the corresponding WGC checkout alive.
      auto source_texture = frame.texture();
      D3D11_TEXTURE2D_DESC source_info{};
      source_texture->GetDesc(&source_info);
      ComPtr<ID3D11Device> source_device;
      source_texture->GetDevice(source_device.GetAddressOf());
      if (source_device.Get() != device_.Get() ||
          source_info.Width < content.width || source_info.Height < content.height ||
          source_info.Format != format_) {
        return std::unexpected{bridge_error(core::AppErrorCode::InvalidValue)};
      }

      const D3D11_BOX source_box{
        .left = 0, .top = 0, .front = 0,
        .right = content.width, .bottom = content.height, .back = 1,
      };
      auto &pending = slot->pending.emplace();
      pending.frame.emplace(std::move(frame));
      pending.content = content;
      pending.stamp = stamp;
      BridgeSubmitResult result;
      if (external != nullptr) {
        if (external->target == (std::numeric_limits<std::uint64_t>::max)()) {
          slot->pending.reset();
          result.count_as_dropped = true;
          result.fallback_requested = true;
          return result;
        }
        pending.external_destination = external->destination;
        pending.external_fence = external->fence;
        pending.target = external->target;
        pending.proof = CopyProof::FencePending;
        context_->CopySubresourceRegion(pending.external_destination.Get(),
                                       0, 0, 0, 0, source_texture.Get(), 0,
                                       &source_box);
        result.d3_copy_bytes =
          static_cast<std::uint64_t>(content.width) * content.height * bytes_per_pixel;
        const auto signal_result =
          external->signal_context->Signal(pending.external_fence.Get(), pending.target);
        if (FAILED(signal_result)) {
          // Copy precedes Signal. Healthy-device failure therefore appends query
          // proof and retains the checkout while the caller requests fallback.
          if (FAILED(device_->GetDeviceRemovedReason())) {
            d3_device_removed_ = true;
            slot->pending.reset();
            result.count_as_dropped = true;
            result.d3_device_removed = true;
            return result;
          }
          context_->End(slot->query.Get());
          context_->Flush();
          pending.proof = CopyProof::QueryPending;
          result.fallback_requested = true;
        }
        else {
          context_->Flush();
        }
      }
      else {
        context_->CopySubresourceRegion(slot->staging.Get(), 0, 0, 0, 0,
                                       source_texture.Get(), 0, &source_box);
        result.d3_copy_bytes =
          static_cast<std::uint64_t>(content.width) * content.height * bytes_per_pixel;
        context_->End(slot->query.Get());
        context_->Flush();
      }
      result.disposition = BridgeSubmitDisposition::Pending;
      return result;
    }

    [[nodiscard]] BridgeProgress
    progress(const std::chrono::steady_clock::time_point now,
             const bool discard_completed, const CpuConsumer &consume) noexcept
    {
      BridgeProgress result;
      bool discard_remaining = discard_completed;
      for (auto &slot : slots_) {
        if (!slot.pending) {
          continue;
        }
        if (d3_device_removed_) {
          finish(slot, BridgeCompletionKind::SafeDiscard, false, result);
          continue;
        }
        auto &pending = *slot.pending;
        if (pending.proof == CopyProof::FencePending) {
          const auto completion = render::classify_d3_completed_value(
            pending.external_fence->GetCompletedValue(), pending.target);
          if (completion == render::D3CompletionStatus::Pending) {
            continue;
          }
          if (completion == render::D3CompletionStatus::DeviceRemoved) {
            d3_device_removed_ = true;
            finish(slot, BridgeCompletionKind::SafeDiscard, false, result);
            continue;
          }
          finish(slot, discard_remaining ? BridgeCompletionKind::SafeDiscard
                                         : BridgeCompletionKind::ExternalReady,
                 discard_remaining, result);
          continue;
        }
        if (pending.proof == CopyProof::QueryPending) {
          result.fallback_requested =
            result.fallback_requested || pending.external_destination != nullptr;
          BOOL complete{};
          const auto query_result = context_->GetData(
            slot.query.Get(), &complete, sizeof(complete),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
          if (query_result == S_FALSE ||
              (query_result == S_OK && complete == FALSE)) {
            continue;
          }
          if (FAILED(query_result)) {
            if (FAILED(device_->GetDeviceRemovedReason())) {
              d3_device_removed_ = true;
              finish(slot, BridgeCompletionKind::SafeDiscard, false, result);
            }
            else {
              terminal_error_ =
                d3_error(core::AppErrorCode::RuntimeStateLost, query_result);
            }
            // An unexplained query error is not copy proof. Keep its checkout
            // and return another deadline, including after terminal admission.
            continue;
          }
          pending.frame.reset();
          result.made_progress = true;
          if (pending.external_destination) {
            finish(slot, BridgeCompletionKind::SafeDiscard, true, result);
            continue;
          }
          pending.proof = CopyProof::CpuMapPending;
        }
        if (discard_remaining) {
          finish(slot, BridgeCompletionKind::SafeDiscard, true, result);
          continue;
        }
        consume_cpu(slot, consume, result, discard_remaining);
      }
      result.pending_inputs = has_pending_inputs();
      if (result.pending_inputs) {
        result.next_deadline = now + pending_poll_interval;
      }
      result.d3_device_removed = d3_device_removed_;
      result.terminal_error = terminal_error_;
      return result;
    }

    [[nodiscard]] bool has_pending_inputs() const noexcept
    {
      return std::ranges::any_of(slots_, [](const ProducerSlot &slot) noexcept {
        return slot.pending.has_value();
      });
    }

    void note_d3_device_removed() noexcept { d3_device_removed_ = true; }

  private:
    [[nodiscard]] ProducerSlot *find_slot(const render::SlotKey key) noexcept
    {
      if (!render::matches_slot_key(key, ring_serial_, slots_.size())) {
        return nullptr;
      }
      return &slots_[key.index];
    }

    [[nodiscard]] std::expected<void, core::AppError>
    create_query(ProducerSlot &slot) noexcept
    {
      D3D11_QUERY_DESC query_info{};
      query_info.Query = D3D11_QUERY_EVENT;
      const auto result = device_->CreateQuery(&query_info, slot.query.GetAddressOf());
      if (FAILED(result)) {
        return std::unexpected{
          d3_error(core::AppErrorCode::RuntimeConstructionFailure, result)};
      }
      return {};
    }

    // One pass visits each fixed slot once, so completing a record cannot exceed
    // batch capacity. Capture values before releasing its borrowed-copy owners.
    void finish(ProducerSlot &slot, const BridgeCompletionKind kind,
                const bool count_as_dropped, BridgeProgress &result,
                const std::optional<core::AppError> error = std::nullopt) noexcept
    {
      if (!slot.pending || result.completion_count == result.completions.size()) {
        std::terminate();
      }
      const auto &pending = *slot.pending;
      result.completions[result.completion_count++] = {
        .key = slot.key,
        .kind = kind,
        .content = pending.content,
        .stamp = pending.stamp,
        .external_fence_target = pending.target,
        .count_as_dropped = count_as_dropped,
        .error = error,
      };
      slot.pending.reset();
      result.made_progress = true;
    }

    void consume_cpu(ProducerSlot &slot, const CpuConsumer &consume,
                     BridgeProgress &result, bool &discard_remaining) noexcept
    {
      D3D11_MAPPED_SUBRESOURCE mapped{};
      const auto map_result = context_->Map(
        slot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (map_result == DXGI_ERROR_WAS_STILL_DRAWING) {
        return;
      }
      if (FAILED(map_result)) {
        if (FAILED(device_->GetDeviceRemovedReason())) {
          d3_device_removed_ = true;
          finish(slot, BridgeCompletionKind::SafeDiscard, false, result);
        }
        else {
          terminal_error_ = d3_error(core::AppErrorCode::RuntimeStateLost, map_result);
          finish(slot, BridgeCompletionKind::SafeDiscard, false, result, terminal_error_);
        }
        return;
      }
      const MappedReadback unmap{*context_.Get(), *slot.staging.Get()};
      const auto &pending = *slot.pending;
      const auto row_bytes =
        static_cast<std::size_t>(pending.content.width) * bytes_per_pixel;
      const auto last_row = static_cast<std::size_t>(pending.content.height - 1);
      constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
      if (mapped.pData == nullptr || mapped.RowPitch < row_bytes ||
          (last_row != 0 && mapped.RowPitch > (maximum - row_bytes) / last_row)) {
        terminal_error_ = bridge_error(core::AppErrorCode::RuntimeStateLost);
        finish(slot, BridgeCompletionKind::SafeDiscard, false, result, terminal_error_);
        return;
      }
      const render::CpuRowsView rows{
        .bytes = {static_cast<const std::byte *>(mapped.pData),
                  last_row * mapped.RowPitch + row_bytes},
        .row_pitch = mapped.RowPitch,
        .content = pending.content,
        .allocation = allocation_,
      };
      std::expected<void, core::AppError> consumed;
      try {
        consumed = consume(slot.key, rows, pending.stamp);
      } catch (const core::AppError &error) {
        consumed = std::unexpected{error};
      } catch (const std::bad_alloc &) {
        consumed = std::unexpected{
          d3_error(core::AppErrorCode::RuntimeStateLost, E_OUTOFMEMORY)};
      } catch (...) {
        consumed = std::unexpected{bridge_error(core::AppErrorCode::RuntimeStateLost)};
      }
      if (!consumed) {
        // Copy proof was established before Map. A consumer error can safely
        // discard this input, but never discharges another slot's pending copy.
        finish(slot, BridgeCompletionKind::CpuConsumerFailed, false, result,
               consumed.error());
        discard_remaining = true;
        return;
      }
      finish(slot, BridgeCompletionKind::CpuConsumed, false, result);
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    std::array<ProducerSlot, slot_count> slots_{};
    std::uint64_t ring_serial_{};
    std::uint64_t last_issued_serial_{};
    std::uint64_t generation_{};
    core::Extent allocation_{};
    DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
    bool d3_device_removed_{};
    std::optional<core::AppError> terminal_error_;
  };

  WindowsFrameBridge::WindowsFrameBridge() : implementation_{std::make_unique<Impl>()} {}
  WindowsFrameBridge::~WindowsFrameBridge() noexcept = default;

  ID3D11Device &WindowsFrameBridge::capture_device() noexcept
  {
    return implementation_->capture_device();
  }

  std::expected<void, core::AppError>
  WindowsFrameBridge::start_ring(const std::uint64_t serial,
                                 const std::uint64_t generation,
                                 const core::Extent allocation,
                                 const DXGI_FORMAT format) noexcept
  {
    return implementation_->start_ring(serial, generation, allocation, format);
  }

  std::expected<void, core::AppError>
  WindowsFrameBridge::create_cpu_slot(const render::SlotKey key) noexcept
  {
    return implementation_->create_cpu_slot(key);
  }

  std::expected<void, core::AppError>
  WindowsFrameBridge::create_query_slot(const render::SlotKey key) noexcept
  {
    return implementation_->create_query_slot(key);
  }

  std::expected<void, core::AppError>
  WindowsFrameBridge::release_ring(const std::uint64_t expected_serial) noexcept
  {
    return implementation_->release_ring(expected_serial);
  }

  std::expected<BridgeSubmitResult, core::AppError>
  WindowsFrameBridge::submit(const render::SlotKey key, WindowsCapturedFrame &&frame,
                             const render::FrameStamp stamp,
                             const ExternalCopyBorrow *const external)
  {
    return implementation_->submit(key, std::move(frame), stamp, external);
  }

  BridgeProgress WindowsFrameBridge::progress(
    const std::chrono::steady_clock::time_point now, const bool discard_completed,
    const CpuConsumer &consume) noexcept
  {
    return implementation_->progress(now, discard_completed, consume);
  }

  bool WindowsFrameBridge::has_pending_inputs() const noexcept
  {
    return implementation_->has_pending_inputs();
  }

  void WindowsFrameBridge::note_d3_device_removed() noexcept
  {
    implementation_->note_d3_device_removed();
  }

}    // namespace avb::platform::windows
