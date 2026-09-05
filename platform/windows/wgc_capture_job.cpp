/// @file
/// @brief One WGC session/pool generation, contained intake, and nonblocking stop.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/wgc_capture_job.hpp"

#include "platform/windows/detail/frame_intake_gate.hpp"
#include "platform/windows/detail/wgc_capture_state.hpp"

#include "airelinna/core/latest_value_mailbox.hpp"

#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <new>
#include <utility>

namespace airelinna::platform::windows {

  namespace {

    using Direct3DDevice =
      winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
    using FramePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
    using PixelFormat = winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

    constexpr PixelFormat pool_format = PixelFormat::B8G8R8A8UIntNormalized;

    [[nodiscard]] core::AppError
    construction_error(const std::int64_t native_detail = E_FAIL) noexcept
    {
      return {.code = core::AppErrorCode::RuntimeConstructionFailure,
              .native_detail = native_detail};
    }

    [[nodiscard]] core::AppError
    platform_error(const std::int64_t native_detail = E_FAIL) noexcept
    {
      return {.code = core::AppErrorCode::PlatformOperationFailure,
              .native_detail = native_detail};
    }

    [[nodiscard]] Direct3DDevice make_direct3d_device(ID3D11Device &device)
    {
      Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
      winrt::check_hresult(device.QueryInterface(IID_PPV_ARGS(&dxgi_device)));

      winrt::com_ptr<::IInspectable> inspectable;
      winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(),
                                                                inspectable.put()));
      return inspectable.as<Direct3DDevice>();
    }

    [[nodiscard]] core::Extent
    checked_extent(const winrt::Windows::Graphics::SizeInt32 size)
    {
      if (size.Width <= 0 || size.Height <= 0) {
        throw winrt::hresult_invalid_argument{};
      }
      return {.width = static_cast<std::uint32_t>(size.Width),
              .height = static_cast<std::uint32_t>(size.Height)};
    }

    [[nodiscard]] winrt::Windows::Graphics::SizeInt32
    native_extent(const core::Extent extent)
    {
      constexpr auto maximum =
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
      if (extent.width == 0 || extent.height == 0 || extent.width > maximum ||
          extent.height > maximum) {
        throw winrt::hresult_invalid_argument{};
      }
      return {.Width = static_cast<std::int32_t>(extent.width),
              .Height = static_cast<std::int32_t>(extent.height)};
    }

    // Shared callback state owns the pool before mailbox/checkouts in declaration order.
    // Late delegates weak-lock this state rather than a destroyed job or runtime.
    class FrameCallbackState final
        : public std::enable_shared_from_this<FrameCallbackState> {
    public:
      FrameCallbackState(FramePool frame_pool, const std::uint32_t buffer_count, const core::Extent allocation, const std::uint64_t generation, WgcCaptureJob::WakeMainLoop wake_main_loop)
          : frame_pool_{std::move(frame_pool)},
            policy_{buffer_count, allocation, generation, pool_format,
                    std::move(wake_main_loop)},
            buffer_count_{buffer_count},
            generation_{generation} {}

      [[nodiscard]] winrt::event_token register_arrival()
      {
        // Capture the registration epoch; revoked/replaced delegates cannot retarget.
        const auto epoch = policy_.current_epoch();
        const std::weak_ptr<FrameCallbackState> weak_state{shared_from_this()};
        return frame_pool_.FrameArrived(
          [weak_state, epoch](const auto &, const auto &) noexcept {
            if (const auto state = weak_state.lock()) {
              state->on_arrival(epoch);
            }
          });
      }

      void revoke_arrival(const winrt::event_token token)
      {
        frame_pool_.FrameArrived(token);
      }

      void on_arrival(const std::uint64_t epoch) noexcept
      {
        auto admission = policy_.enter(epoch);
        if (!admission) {
          return;
        }
        // Early matching arrivals preserve one bit without touching the native sender/pool.
        if (!admission->is_open()) {
          admission->record_pending();
          admission->wake_if_open();
          return;
        }
        static_cast<void>(intake_one(*admission));
      }

      void service_pending_arrival() noexcept
      {
        if (!policy_.claim_pending_arrival()) {
          return;
        }
        auto admission = policy_.enter(policy_.current_epoch());
        if (!admission || !admission->is_open()) {
          return;
        }
        // Bound each service pass even when new native frames keep arriving.
        for (std::uint32_t index = 0; index < policy_.pool_buffer_count();
             ++index) {
          if (!intake_one(*admission)) {
            break;
          }
        }
      }

      [[nodiscard]] std::optional<WindowsCapturedFrame> take_latest()
      {
        return mailbox_.take();
      }

      [[nodiscard]] bool has_latest() const noexcept { return !mailbox_empty(); }

      [[nodiscard]] CaptureSignalBatch drain_signals()
      {
        return policy_.drain_signals();
      }

      [[nodiscard]] std::uint64_t close_for_reconfigure() noexcept
      {
        return policy_.close_for_reconfigure();
      }

      [[nodiscard]] bool callbacks_quiescent() const noexcept
      {
        return policy_.frame_callbacks_quiescent();
      }

      [[nodiscard]] bool mailbox_empty() const noexcept
      {
        if (mailbox_closed_) {
          return true;
        }
        try {
          const auto counters = mailbox_.counters();
          return counters.submitted == counters.replaced + counters.taken;
        } catch (...) {
          return false;
        }
      }

      [[nodiscard]] bool mailbox_closed_and_empty() const noexcept
      {
        return mailbox_closed_ && mailbox_empty();
      }

      void recreate(const Direct3DDevice &direct3d_device,
                    const core::Extent allocation,
                    const std::uint64_t generation)
      {
        frame_pool_.Recreate(direct3d_device, pool_format,
                             static_cast<std::int32_t>(buffer_count_),
                             native_extent(allocation));
        const auto prepared = policy_.prepare_current_epoch(allocation, generation);
        if (!prepared) {
          throw winrt::hresult_illegal_method_call{};
        }
        generation_ = generation;
      }

      void open_current_epoch() noexcept { policy_.open_current_epoch(); }

      void request_terminal_stop() noexcept
      {
        policy_.request_terminal_stop();
        try {
          mailbox_.close();
          mailbox_closed_ = true;
        } catch (...) {
          mailbox_closed_ = false;
        }
      }

    private:
      // Own immediately after TryGetNextFrame, before metadata projection can throw.
      class PendingFrameCheckout final {
      public:
        explicit PendingFrameCheckout(
          winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame
            frame) noexcept
            : frame_{std::move(frame)} {}

        ~PendingFrameCheckout() noexcept { close(); }

        PendingFrameCheckout(const PendingFrameCheckout &) = delete;
        PendingFrameCheckout &operator=(const PendingFrameCheckout &) = delete;

        [[nodiscard]] auto &frame() noexcept { return frame_; }

        [[nodiscard]] WindowsCapturedFrame finish(const std::uint64_t sequence,
                                                  const std::uint64_t generation,
                                                  const core::Extent allocation,
                                                  const core::Extent content)
        {
          auto frame = std::exchange(frame_, nullptr);
          return WindowsCapturedFrame{
            sequence, generation, allocation,
            content, pool_format, std::chrono::steady_clock::now(),
            std::move(frame)};
        }

      private:
        void close() noexcept
        {
          auto frame = std::exchange(frame_, nullptr);
          if (!frame) {
            return;
          }
          try {
            frame.Close();
          } catch (...) {
          }
        }

        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame_{nullptr};
      };

      struct AcquiredFrame final {
        WindowsCapturedFrame checkout;
        detail::FrameObservation observation;
      };

      [[nodiscard]] std::optional<AcquiredFrame> try_take_native_frame()
      {
        // Gate ownership spans this pull through publication to preserve frame order.
        auto raw_frame = frame_pool_.TryGetNextFrame();
        if (!raw_frame) {
          return std::nullopt;
        }
        PendingFrameCheckout pending{std::move(raw_frame)};

        const auto content = checked_extent(pending.frame().ContentSize());
        const auto access = pending.frame()
                              .Surface()
                              .as<::Windows::Graphics::DirectX::Direct3D11::
                                    IDirect3DDxgiInterfaceAccess>();
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        winrt::check_hresult(
          access->GetInterface(IID_PPV_ARGS(texture.GetAddressOf())));
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        const core::Extent observed_allocation{.width = description.Width,
                                               .height = description.Height};
        const auto observed_format =
          description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? pool_format
                                                           : PixelFormat::Unknown;
        const auto sequence =
          next_sequence_.fetch_add(1, std::memory_order_relaxed);
        const auto copyable_content =
          detail::copyable_content_extent(content, observed_allocation);
        return AcquiredFrame{
          .checkout = pending.finish(sequence, generation_, observed_allocation,
                                     copyable_content),
          .observation = {.resource_generation = generation_,
                          .allocation_extent = observed_allocation,
                          .content_size = content,
                          .surface_format = observed_format}};
      }

      [[nodiscard]] bool
      intake_one(detail::FrameCallbackPolicy::Admission &admission) noexcept
      {
        // Contention is remembered atomically; no callback waits for another native pull.
        if (!intake_gate_.try_claim()) {
          return false;
        }
        const auto accepted = intake_claimed_frame();
        if (intake_gate_.release()) {
          // Release before rearming so main-loop service can make progress.
          // The admission still protects the borrowed wake target here.
          admission.record_pending();
          admission.wake_if_open();
        }
        return accepted;
      }

      [[nodiscard]] bool intake_claimed_frame() noexcept
      {
        try {
          auto acquired = try_take_native_frame();
          if (!acquired) {
            return false;
          }
          const auto observed = policy_.observe_frame(acquired->observation);
          if (!observed.accepted) {
            discard_latest();
            return false;
          }
          // Replacement returns the stale checkout; only readiness/signal edges need wake.
          const auto result = mailbox_.submit(std::move(acquired->checkout));
          if (observed.signal_edge ||
              result == FrameMailbox::SubmitResult::BecameReady) {
            policy_.notify_mailbox_ready();
          }
          return result != FrameMailbox::SubmitResult::Closed;
        } catch (const winrt::hresult_error &error) {
          policy_.record_projection_failure(error.code().value);
        } catch (const std::bad_alloc &) {
          policy_.record_out_of_memory();
        } catch (...) {
          policy_.record_unknown_callback_failure();
        }
        discard_latest();
        return false;
      }

      void discard_latest() noexcept
      {
        try {
          static_cast<void>(mailbox_.take());
        } catch (...) {
        }
      }

      // Reverse member destruction releases the mailbox before the owning frame pool.
      using FrameMailbox = core::LatestValueMailbox<WindowsCapturedFrame>;
      FramePool frame_pool_{nullptr};
      detail::FrameCallbackPolicy policy_;
      detail::FrameIntakeGate intake_gate_;
      FrameMailbox mailbox_;
      const std::uint32_t buffer_count_;
      std::uint64_t generation_{};
      std::atomic_uint64_t next_sequence_{1};
      bool mailbox_closed_{};
    };

  }    // namespace

  // Runtime retains this owner through stop; only inactive construction may unwind early.
  class WgcCaptureJob::Impl final {
  public:
    Impl(WindowsCaptureSource &source, Direct3DDevice direct3d_device, winrt::Windows::Graphics::Capture::GraphicsCaptureSession session, std::shared_ptr<FrameCallbackState> frame_state, const winrt::event_token frame_arrived_token)
        : source_{source}, direct3d_device_{std::move(direct3d_device)}, session_{std::move(session)}, frame_state_{std::move(frame_state)}, frame_arrived_token_{frame_arrived_token}, token_registered_{true} {}

    ~Impl() noexcept
    {
      request_stop();
      // Releasing a session or pool after uncertain cleanup, or while a callback
      // remains entered, violates the runtime ownership contract. Terminate
      // before member destruction; the runtime must retain the job until its
      // concrete stop predicate succeeds. An inactive factory partial has never
      // opened frame admission and may unwind without a published tick loop.
      if (publication_.started()) {
        detail::enforce_wgc_job_destruction_invariant(stop_quiescent());
      }
    }

    [[nodiscard]] bool has_latest() const noexcept
    {
      return frame_state_->has_latest();
    }

    [[nodiscard]] std::optional<WindowsCapturedFrame> take_latest()
    {
      return frame_state_->take_latest();
    }

    [[nodiscard]] CaptureSignalBatch drain_signals()
    {
      return frame_state_->drain_signals();
    }

    void service_pending_arrival() noexcept
    {
      frame_state_->service_pending_arrival();
    }

    [[nodiscard]] std::expected<void, core::AppError> start_for_publication()
    {
      return publication_.start_for_publication(
        [this] noexcept { return source_.is_closed(); },
        [this] { session_.StartCapture(); },
        [this] noexcept { frame_state_->open_current_epoch(); });
    }

    [[nodiscard]] std::expected<void, core::AppError> begin_reconfigure()
    {
      if (stopping_ || reconfigure_pending_) {
        return std::unexpected{
          core::AppError{.code = core::AppErrorCode::InvalidCommand}};
      }
      static_cast<void>(frame_state_->close_for_reconfigure());
      reconfigure_pending_ = true;
      if (!token_registered_) {
        return {};
      }
      try {
        frame_state_->revoke_arrival(frame_arrived_token_);
        token_registered_ = false;
        return {};
      } catch (const winrt::hresult_error &error) {
        return std::unexpected{platform_error(error.code().value)};
      } catch (const std::bad_alloc &) {
        return std::unexpected{platform_error(E_OUTOFMEMORY)};
      } catch (...) {
        return std::unexpected{platform_error()};
      }
    }

    [[nodiscard]] bool frame_callbacks_quiescent() const noexcept
    {
      return frame_state_->callbacks_quiescent();
    }

    [[nodiscard]] std::expected<void, core::AppError>
    prepare_reconfigure(const core::Extent allocation,
                        const std::uint64_t generation)
    {
      if (stopping_ || !reconfigure_pending_ || token_registered_ ||
          !frame_state_->callbacks_quiescent() ||
          !frame_state_->mailbox_empty()) {
        return std::unexpected{
          core::AppError{.code = core::AppErrorCode::InvalidCommand}};
      }
      try {
        frame_state_->recreate(direct3d_device_, allocation, generation);
        frame_arrived_token_ = frame_state_->register_arrival();
        token_registered_ = true;
        prepared_ = true;
        return {};
      } catch (const winrt::hresult_error &error) {
        return std::unexpected{construction_error(error.code().value)};
      } catch (const std::bad_alloc &) {
        return std::unexpected{construction_error(E_OUTOFMEMORY)};
      } catch (...) {
        return std::unexpected{construction_error()};
      }
    }

    void commit_reconfigure() noexcept
    {
      if (stopping_ || !reconfigure_pending_ || !prepared_) {
        return;
      }
      prepared_ = false;
      reconfigure_pending_ = false;
      frame_state_->open_current_epoch();
    }

    void request_stop() noexcept
    {
      if (stopping_) {
        return;
      }
      stopping_ = true;
      // Close admission/mailbox before revoke/Close races already-entered delegates.
      frame_state_->request_terminal_stop();
      stop_state_.request_stop(
        [this] {
          if (token_registered_) {
            frame_state_->revoke_arrival(frame_arrived_token_);
            token_registered_ = false;
          }
        },
        [this] { session_.Close(); });
    }

    [[nodiscard]] bool stop_quiescent() const noexcept
    {
      return stop_state_.stop_quiescent(frame_state_->callbacks_quiescent(),
                                        frame_state_->mailbox_closed_and_empty());
    }

    [[nodiscard]] std::optional<CaptureCleanupFailure>
    cleanup_failure() const noexcept
    {
      return stop_state_.cleanup_failure();
    }

  private:
    WindowsCaptureSource &source_;
    Direct3DDevice direct3d_device_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    std::shared_ptr<FrameCallbackState> frame_state_;
    winrt::event_token frame_arrived_token_{};
    bool token_registered_{};
    bool reconfigure_pending_{};
    bool prepared_{};
    bool stopping_{};
    detail::PublicationGate publication_;
    detail::CaptureJobStopState stop_state_;
  };

  std::expected<std::unique_ptr<WgcCaptureJob>, core::AppError>
  WgcCaptureJob::create_inactive(WindowsCaptureSource &source,
                                 ID3D11Device &d3d_device,
                                 const std::uint64_t resource_generation,
                                 const std::uint32_t pool_buffer_count,
                                 WakeMainLoop wake_main_loop)
  {
    if (source.is_closed()) {
      return std::unexpected{
        core::AppError{.code = core::AppErrorCode::SourceUnavailable}};
    }
    if (pool_buffer_count == 0 ||
        pool_buffer_count > static_cast<std::uint32_t>(
                              std::numeric_limits<std::int32_t>::max()) ||
        !wake_main_loop) {
      return std::unexpected{construction_error(E_INVALIDARG)};
    }

    try {
      auto direct3d_device = make_direct3d_device(d3d_device);
      const auto allocation = checked_extent(source.item().Size());
      // Free-threaded dispatch needs explicit admission, intake, and ownership guards.
      auto frame_pool = FramePool::CreateFreeThreaded(
        direct3d_device, pool_format,
        static_cast<std::int32_t>(pool_buffer_count),
        native_extent(allocation));
      auto session = frame_pool.CreateCaptureSession(source.item());
      auto frame_state = std::make_shared<FrameCallbackState>(
        std::move(frame_pool), pool_buffer_count, allocation,
        resource_generation, std::move(wake_main_loop));
      const auto token = frame_state->register_arrival();
      auto implementation = std::make_unique<Impl>(
        source, std::move(direct3d_device), std::move(session),
        std::move(frame_state), token);
      return std::unique_ptr<WgcCaptureJob>{
        new WgcCaptureJob{std::move(implementation), allocation}};
    } catch (const winrt::hresult_error &error) {
      const auto native_detail = error.code().value;
      return std::unexpected{detail::classify_wgc_construction_failure(
        source.is_closed(), native_detail, native_detail == E_ACCESSDENIED)};
    } catch (const std::bad_alloc &) {
      return std::unexpected{detail::classify_wgc_construction_failure(
        source.is_closed(), E_OUTOFMEMORY)};
    } catch (...) {
      return std::unexpected{
        detail::classify_wgc_construction_failure(source.is_closed(), E_FAIL)};
    }
  }

  WgcCaptureJob::WgcCaptureJob(std::unique_ptr<Impl> implementation, const core::Extent initial_allocation) noexcept
      : implementation_{std::move(implementation)}, initial_allocation_{initial_allocation} {}

  core::Extent WgcCaptureJob::initial_allocation() const noexcept
  {
    return initial_allocation_;
  }

  WgcCaptureJob::~WgcCaptureJob() = default;

  std::optional<WindowsCapturedFrame> WgcCaptureJob::take_latest()
  {
    return implementation_->take_latest();
  }

  bool WgcCaptureJob::has_latest() const noexcept
  {
    return implementation_->has_latest();
  }

  CaptureSignalBatch WgcCaptureJob::drain_signals()
  {
    return implementation_->drain_signals();
  }

  void WgcCaptureJob::service_pending_arrival() noexcept
  {
    implementation_->service_pending_arrival();
  }

  std::expected<void, core::AppError> WgcCaptureJob::start_for_publication()
  {
    return implementation_->start_for_publication();
  }

  std::expected<void, core::AppError> WgcCaptureJob::begin_reconfigure()
  {
    return implementation_->begin_reconfigure();
  }

  bool WgcCaptureJob::frame_callbacks_quiescent() const noexcept
  {
    return implementation_->frame_callbacks_quiescent();
  }

  std::expected<void, core::AppError>
  WgcCaptureJob::prepare_reconfigure(const core::Extent allocation,
                                     const std::uint64_t generation)
  {
    return implementation_->prepare_reconfigure(allocation, generation);
  }

  void WgcCaptureJob::commit_reconfigure() noexcept
  {
    implementation_->commit_reconfigure();
  }

  void WgcCaptureJob::request_stop() noexcept { implementation_->request_stop(); }

  bool WgcCaptureJob::stop_quiescent() const noexcept
  {
    return implementation_->stop_quiescent();
  }

  std::optional<CaptureCleanupFailure>
  WgcCaptureJob::cleanup_failure() const noexcept
  {
    return implementation_->cleanup_failure();
  }

}    // namespace airelinna::platform::windows
