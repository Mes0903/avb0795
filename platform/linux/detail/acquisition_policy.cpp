/// @file
/// @brief Protocol result classification without inventing permission deadlines.
#include "platform/linux/detail/acquisition_policy.hpp"
namespace avb::linux_platform::detail {
ResponseDisposition response_disposition(unsigned code) noexcept
{
  if (code == 0) return ResponseDisposition::Success;
  if (code == 1) return ResponseDisposition::Cancelled;
  return ResponseDisposition::Failure;
}
std::optional<std::uint64_t> stream_serial(unsigned version, std::optional<std::uint64_t> serial) noexcept
{ return version >= 6 ? serial : std::nullopt; }
}
