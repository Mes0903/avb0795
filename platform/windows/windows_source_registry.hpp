#ifndef AIRELINNA_PLATFORM_WINDOWS_WINDOWS_SOURCE_REGISTRY_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WINDOWS_SOURCE_REGISTRY_HPP

#include "airelinna/core/types.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace airelinna::platform::windows {

  namespace detail {
    class ItemObservationState;
    struct WindowsSourceRegistryControl;

    class WindowsSourceIdGeneration final {
    public:
      WindowsSourceIdGeneration();

      [[nodiscard]] core::SourceId issue();
      void advance();
      [[nodiscard]] std::expected<void, core::AppError>
      validate(const core::SourceId &id) const noexcept;

    private:
      void rebuild_prefix();

      std::uint64_t generation_{};
      std::uint64_t next_ordinal_{};
      std::string prefix_;
    };

  }    // namespace detail

  class WindowsCaptureSource;

  class WindowsSourceLease final {
  public:
    ~WindowsSourceLease();

    WindowsSourceLease(const WindowsSourceLease &) = delete;
    WindowsSourceLease &operator=(const WindowsSourceLease &) = delete;
    WindowsSourceLease(WindowsSourceLease &&other) noexcept;
    WindowsSourceLease &operator=(WindowsSourceLease &&other) noexcept;

  private:
    friend class WindowsCaptureSource;
    friend class WindowsSourceRegistry;
    [[nodiscard]] detail::ItemObservationState &observation() const noexcept;
    WindowsSourceLease(
      std::shared_ptr<detail::WindowsSourceRegistryControl> control,
      std::shared_ptr<detail::ItemObservationState> observation) noexcept;
    void release() noexcept;

    std::shared_ptr<detail::WindowsSourceRegistryControl> control_;
    std::shared_ptr<detail::ItemObservationState> observation_;
  };

  class WindowsSourceRegistry final {
  public:
    class Refresh final {
    public:
      ~Refresh();

      Refresh(const Refresh &) = delete;
      Refresh &operator=(const Refresh &) = delete;
      Refresh(Refresh &&other) noexcept;
      Refresh &operator=(Refresh &&other) noexcept;

      [[nodiscard]] std::expected<std::vector<core::SourceId>, core::AppError>
      commit(std::vector<std::shared_ptr<detail::ItemObservationState>>
               observations);

    private:
      friend class WindowsSourceRegistry;
      explicit Refresh(WindowsSourceRegistry &owner) noexcept;
      void cancel() noexcept;

      WindowsSourceRegistry *owner_{};
    };

    WindowsSourceRegistry();
    ~WindowsSourceRegistry();

    WindowsSourceRegistry(const WindowsSourceRegistry &) = delete;
    WindowsSourceRegistry &operator=(const WindowsSourceRegistry &) = delete;
    WindowsSourceRegistry(WindowsSourceRegistry &&) = delete;
    WindowsSourceRegistry &operator=(WindowsSourceRegistry &&) = delete;

    [[nodiscard]] std::expected<Refresh, core::AppError> begin_refresh();
    [[nodiscard]] std::expected<WindowsSourceLease, core::AppError>
    resolve(const core::SourceId &id) const;

  private:
    [[nodiscard]] std::expected<std::vector<core::SourceId>, core::AppError>
    commit_refresh(
      std::vector<std::shared_ptr<detail::ItemObservationState>> observations);
    void cancel_refresh() noexcept;

    std::shared_ptr<detail::WindowsSourceRegistryControl> control_;
  };

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_SOURCE_REGISTRY_HPP
