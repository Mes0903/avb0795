#include "avb/core/latest_value_mailbox.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace {

  struct LifetimeCounts {
    std::uint64_t alive{};
    std::uint64_t destroyed{};
  };

  class MoveOnlyCountedValue final {
  public:
    MoveOnlyCountedValue(int value, std::shared_ptr<LifetimeCounts> counts, std::function<void()> on_destroy = {})
        : value_(value), counts_(std::move(counts)), on_destroy_(std::move(on_destroy))
    {
      ++counts_->alive;
    }

    ~MoveOnlyCountedValue() { release(); }

    MoveOnlyCountedValue(const MoveOnlyCountedValue &) = delete;
    MoveOnlyCountedValue &operator=(const MoveOnlyCountedValue &) = delete;

    MoveOnlyCountedValue(MoveOnlyCountedValue &&other) noexcept
        : value_(std::exchange(other.value_, 0)),
          counts_(std::move(other.counts_)),
          on_destroy_(std::move(other.on_destroy_)) {}

    MoveOnlyCountedValue &operator=(MoveOnlyCountedValue &&other) noexcept
    {
      if (this != &other) {
        release();
        value_ = std::exchange(other.value_, 0);
        counts_ = std::move(other.counts_);
        on_destroy_ = std::move(other.on_destroy_);
      }
      return *this;
    }

    [[nodiscard]] int value() const noexcept { return value_; }

  private:
    void release() noexcept
    {
      if (!counts_) {
        return;
      }

      auto on_destroy = std::move(on_destroy_);
      --counts_->alive;
      ++counts_->destroyed;
      counts_.reset();
      if (on_destroy) {
        on_destroy();
      }
    }

    int value_{};
    std::shared_ptr<LifetimeCounts> counts_;
    std::function<void()> on_destroy_;
  };

}    // namespace

TEST_CASE(
  "avb0795 V1 case 02: newest-only mailbox replacement, close, "
  "and move destruction",
  "[avb0795-v1]")
{
  using Mailbox = avb::core::LatestValueMailbox<MoveOnlyCountedValue>;
  using SubmitResult = Mailbox::SubmitResult;

  SECTION("submission replaces only the pending value and take owns newest")
  {
    auto counts = std::make_shared<LifetimeCounts>();
    Mailbox mailbox;

    CHECK(mailbox.submit(MoveOnlyCountedValue{1, counts}) ==
          SubmitResult::BecameReady);
    CHECK(mailbox.submit(MoveOnlyCountedValue{2, counts}) ==
          SubmitResult::ReplacedPending);
    CHECK(counts->alive == 1);
    CHECK(counts->destroyed == 1);

    const auto before_take = mailbox.counters();
    CHECK(before_take.submitted == 2);
    CHECK(before_take.replaced == 1);
    CHECK(before_take.taken == 0);

    auto newest = mailbox.take();
    REQUIRE(newest.has_value());
    CHECK(newest->value() == 2);
    CHECK_FALSE(mailbox.take().has_value());

    const auto after_take = mailbox.counters();
    CHECK(after_take.submitted == 2);
    CHECK(after_take.replaced == 1);
    CHECK(after_take.taken == 1);

    newest.reset();
    CHECK(counts->alive == 0);
    CHECK(counts->destroyed == 2);
  }

  SECTION("replacement destruction can safely re-enter the mailbox")
  {
    auto counts = std::make_shared<LifetimeCounts>();
    Mailbox mailbox;
    bool reentered{};

    CHECK(mailbox.submit(MoveOnlyCountedValue{1, counts, [&] {
                                                reentered =
                                                  mailbox.take().has_value();
                                              }}) == SubmitResult::BecameReady);
    CHECK(mailbox.submit(MoveOnlyCountedValue{2, counts}) ==
          SubmitResult::ReplacedPending);

    CHECK(reentered);
    CHECK_FALSE(mailbox.take().has_value());
    const auto counters = mailbox.counters();
    CHECK(counters.submitted == 2);
    CHECK(counters.replaced == 1);
    CHECK(counters.taken == 1);
    CHECK(counts->alive == 0);
    CHECK(counts->destroyed == 2);
  }

  SECTION("close destroys pending content outside the lock and stays closed")
  {
    auto counts = std::make_shared<LifetimeCounts>();
    Mailbox mailbox;
    bool close_reentered{};

    CHECK(mailbox.submit(MoveOnlyCountedValue{
            3, counts, [&] {
              close_reentered =
                mailbox.submit(MoveOnlyCountedValue{4, counts}) ==
                SubmitResult::Closed;
            }}) == SubmitResult::BecameReady);

    mailbox.close();
    CHECK(close_reentered);
    CHECK_FALSE(mailbox.take().has_value());
    CHECK(mailbox.submit(MoveOnlyCountedValue{5, counts}) ==
          SubmitResult::Closed);
    mailbox.close();

    const auto counters = mailbox.counters();
    CHECK(counters.submitted == 1);
    CHECK(counters.replaced == 0);
    CHECK(counters.taken == 0);
    CHECK(counts->alive == 0);
    CHECK(counts->destroyed == 3);
  }
}
