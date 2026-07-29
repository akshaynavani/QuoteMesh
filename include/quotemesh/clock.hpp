#pragma once

#include <chrono>

#include "quotemesh/types.hpp"

namespace qm {

// Monotonic nanoseconds for intra-process latency measurement.
inline Timestamp mono_ns() noexcept {
  return static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}

// Wall-clock nanoseconds since the Unix epoch (used for exchange timestamps).
inline Timestamp wall_ns() noexcept {
  return static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
}

}  // namespace qm
