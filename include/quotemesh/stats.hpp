#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <string>

#include "quotemesh/types.hpp"

namespace qm {

// Per-feed counters. Written by one feed thread, read by the stats reporter.
struct alignas(kCacheLine) FeedStats {
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<std::uint64_t> messages{0};
  std::atomic<std::uint64_t> parse_errors{0};
  std::atomic<std::uint64_t> gaps{0};          // messages missing according to seq numbers
  std::atomic<std::uint64_t> stale{0};         // duplicate / out-of-order messages dropped
  std::atomic<std::uint64_t> backpressure{0};  // times the queue to the aggregator was full
  std::atomic<std::uint64_t> last_seq{0};
};

// Lock-free log2 latency histogram (single writer, many readers).
class LatencyHistogram {
 public:
  static constexpr std::size_t kBuckets = 48;

  void record(std::uint64_t ns) noexcept {
    const std::size_t b = ns == 0 ? 0 : static_cast<std::size_t>(std::bit_width(ns));
    buckets_[b < kBuckets ? b : kBuckets - 1].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(ns, std::memory_order_relaxed);
  }

  std::uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
  double mean() const noexcept {
    const auto c = count();
    return c ? static_cast<double>(sum_.load(std::memory_order_relaxed)) / static_cast<double>(c)
             : 0.0;
  }

  // Upper bound (ns) of the bucket containing the given percentile (0..1).
  std::uint64_t percentile(double p) const noexcept {
    const auto total = count();
    if (total == 0) return 0;
    const auto target = static_cast<std::uint64_t>(p * static_cast<double>(total));
    std::uint64_t seen = 0;
    for (std::size_t b = 0; b < kBuckets; ++b) {
      seen += buckets_[b].load(std::memory_order_relaxed);
      if (seen > target) return b == 0 ? 0 : (1ull << b) - 1;
    }
    return ~0ull;
  }

 private:
  std::array<std::atomic<std::uint64_t>, kBuckets> buckets_{};
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> sum_{0};
};

struct alignas(kCacheLine) AggregatorStats {
  std::atomic<std::uint64_t> events{0};
  std::atomic<std::uint64_t> quotes{0};
  std::atomic<std::uint64_t> trades{0};
  std::atomic<std::uint64_t> nbbo_changes{0};
  std::atomic<std::uint64_t> crossed{0};
  LatencyHistogram feed_to_book_ns;  // feed-handler parse -> aggregator applied
};

}  // namespace qm
