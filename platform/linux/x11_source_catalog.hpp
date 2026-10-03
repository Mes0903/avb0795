/// @file
/// @brief Checked X11 discovery and lifetime-bound source identities for Linux owners.
/// Discovery owns no capture resources. Only copied, immutable target facts and an
/// atomic retirement token may cross from the catalog thread to a capture worker.
#ifndef AVB0795_PLATFORM_LINUX_X11_SOURCE_CATALOG_HPP
#define AVB0795_PLATFORM_LINUX_X11_SOURCE_CATALOG_HPP

#include "avb/app/ports.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace avb::linux_platform {
  namespace detail {
    class X11CatalogState;
  }

  /// @brief Owned observation snapshot; validity never keeps the native source alive.
  /// Geometry is immutable in each copy; resolve again for current geometry. The
  /// token is shared solely for atomic invalidation and remains safe after catalog
  /// destruction. Readers may call valid() on any thread, but must also watch their
  /// own native resource: catalog invalidation advances only when its owner polls.
  class X11SourceTarget {
  public:
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] core::SourceKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::uint32_t drawable() const noexcept { return drawable_; }
    [[nodiscard]] std::uint32_t root() const noexcept { return root_; }
    [[nodiscard]] std::uint32_t monitor_name() const noexcept { return monitor_name_; }
    [[nodiscard]] core::PixelRegion region() const noexcept { return region_; }
    [[nodiscard]] const std::string &display_name() const noexcept { return display_name_; }
    [[nodiscard]] int screen_number() const noexcept { return screen_number_; }

  private:
    friend class detail::X11CatalogState;
    std::shared_ptr<std::atomic_bool> live_;
    std::uint64_t generation_{};
    core::SourceKind kind_{};
    std::uint32_t drawable_{}, root_{}, monitor_name_{};
    core::PixelRegion region_{};
    std::string display_name_;
    int screen_number_{};
  };

  namespace detail {
    /// @brief Checked server facts used by both native enumeration and scalar policy.
    /// Window support requires Composite 0.2 and genuine EWMH client-list support;
    /// a valid root independently permits display capture. No interaction support
    /// is inferred from discovery, including the historical opacity default.
    struct X11CatalogProbe {
      bool composite_available{};
      std::uint32_t composite_major{}, composite_minor{};
      bool ewmh_client_list_valid{};
      std::uint32_t root{};
      core::Extent root_extent{};
      bool root_drawable_valid{};
    };

    /// @brief One watched candidate with owning display text and checked geometry.
    /// Window coordinates are capture-local; display coordinates are root-relative.
    /// A nonzero monitor name is a RandR atom, never a list position. Refresh borrows
    /// the observation span only for the call and copies every published value.
    struct X11SourceObservation {
      core::SourceKind kind{};
      std::uint32_t drawable{}, monitor_name{};
      std::string display_label;
      std::int32_t x{}, y{};
      core::Extent extent{};
      bool drawable_valid{}, ewmh_client{};
    };

    [[nodiscard]] std::expected<std::uint64_t, core::AppError>
    next_x11_generation(std::uint64_t last_issued) noexcept;
    [[nodiscard]] core::PlatformCapabilities
    x11_catalog_capabilities(const X11CatalogProbe &probe) noexcept;

    /// @brief Catalog-thread identity policy consumed directly by the native owner.
    /// Rename/window resize retain identity. Removal, Destroy and display-layout
    /// invalidation retire tokens permanently; generation exhaustion poisons the
    /// state. Allocation failure also invalidates all observations rather than
    /// exposing a partially committed refresh. No mutable state is borrowed by
    /// targets; destruction atomically retires their independently owned tokens.
    class X11CatalogState {
    public:
      explicit X11CatalogState(std::string display_name = {}, int screen_number = 0);
      ~X11CatalogState();
      X11CatalogState(const X11CatalogState &) = delete;
      X11CatalogState &operator=(const X11CatalogState &) = delete;
      [[nodiscard]] std::expected<std::vector<core::SourceDescriptor>, core::AppError>
      refresh(const X11CatalogProbe &probe, std::span<const X11SourceObservation> observations, std::span<const std::uint32_t> owned = {});
      [[nodiscard]] std::expected<X11SourceTarget, core::AppError>
      resolve(const core::SourceId &id) const;
      void destroy(std::uint32_t drawable) noexcept;
      void invalidate_displays() noexcept;
      void invalidate_all() noexcept;

    private:
      struct Record {
        core::SourceDescriptor descriptor;
        X11SourceTarget target;
      };
      std::vector<Record> records_;
      std::uint64_t last_issued_{};
      bool exhausted_{};
      std::string display_name_;
      int screen_number_{};
    };
  }    // namespace detail

  /// @brief Main-thread catalog with its own connection, checked watches and queue.
  /// create probes discovery only. All methods except copied-target reads belong
  /// to the creating thread; no connection or mutable catalog borrow reaches a
  /// worker. Destruction/fatal connection loss retires every target before native
  /// teardown. Server I/O is synchronous here, never a runtime tick/capture path.
  class X11SourceCatalog final : public app::ISourceCatalog {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<X11SourceCatalog>, core::AppError> create();
    ~X11SourceCatalog() override;
    X11SourceCatalog(const X11SourceCatalog &) = delete;
    X11SourceCatalog &operator=(const X11SourceCatalog &) = delete;
    [[nodiscard]] std::expected<std::vector<core::SourceDescriptor>, core::AppError> enumerate() override;

    /// @brief Revalidate native eligibility and resolve only the exact observation.
    /// A reply barrier drains queued Destroy before lookup; stale IDs never attach
    /// to a replacement XID. For capture handoff, first resolve, create the inactive
    /// producer on display_name()/screen_number() with its own checked watch, then
    /// resolve the SAME SourceId again before activation. Require the same generation
    /// and a still-valid original token. The producer must process its watch events
    /// before resource admission and throughout capture, including after this call.
    /// This closes the unwatched gap; one initial resolve alone cannot do so.
    [[nodiscard]] std::expected<X11SourceTarget, core::AppError> resolve(const core::SourceId &id);
    [[nodiscard]] core::PlatformCapabilities capabilities() const noexcept;

    /// @brief Exclude one live controller/replica drawable, never its whole process.
    /// The caller must own this exact window through successful registration and
    /// register before exposing it to catalog selection. A checked lifetime watch
    /// retires the exclusion on native Destroy, so later XID reuse is not hidden.
    /// There is no early unregister: exclusion follows the owned window's lifetime.
    [[nodiscard]] std::expected<void, core::AppError> register_owned_window(std::uint32_t drawable);

  private:
    struct Impl;
    explicit X11SourceCatalog(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
  };
}    // namespace avb::linux_platform

#endif    // AVB0795_PLATFORM_LINUX_X11_SOURCE_CATALOG_HPP
