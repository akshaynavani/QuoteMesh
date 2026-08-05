#pragma once

#include <atomic>
#include <cstdint>

#include "quotemesh/shm/layout.hpp"

namespace qm::shm {

// Latest consolidated state per symbol, indexed by SymbolId. One writer (the aggregator),
// any number of readers. Each entry is a classic seqlock: the version is odd while an update
// is in flight; readers retry until they observe the same even version before and after.
class SnapshotWriter {
 public:
  SnapshotWriter() = default;
  SnapshotWriter(SegmentHeader* hdr, SnapshotEntry* entries) noexcept
      : hdr_(hdr), entries_(entries), max_(hdr->max_symbols) {}

  bool write(SymbolId sym, const SnapshotData& d) noexcept {
    if (sym >= max_) return false;
    SnapshotEntry& e = entries_[sym];
    const std::uint64_t v = e.version.load(std::memory_order_relaxed);
    e.version.store(v + 1, std::memory_order_relaxed);  // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    store_words(e.words, d);
    e.version.store(v + 2, std::memory_order_release);  // even: stable
    std::uint32_t count = hdr_->symbol_count.load(std::memory_order_relaxed);
    if (sym + 1 > count) hdr_->symbol_count.store(sym + 1, std::memory_order_release);
    return true;
  }

 private:
  SegmentHeader* hdr_{nullptr};
  SnapshotEntry* entries_{nullptr};
  std::uint32_t max_{0};
};

class SnapshotReader {
 public:
  SnapshotReader() = default;
  SnapshotReader(const SegmentHeader* hdr, const SnapshotEntry* entries) noexcept
      : hdr_(hdr), entries_(entries) {}

  std::uint32_t symbol_count() const noexcept {
    return hdr_->symbol_count.load(std::memory_order_acquire);
  }

  // Returns false if the symbol has never been written. Spins (bounded) on concurrent writes.
  bool read(SymbolId sym, SnapshotData& out) const noexcept {
    if (sym >= hdr_->max_symbols) return false;
    const SnapshotEntry& e = entries_[sym];
    for (int attempt = 0; attempt < 1'000'000; ++attempt) {
      const std::uint64_t v1 = e.version.load(std::memory_order_acquire);
      if (v1 == 0) return false;
      if (v1 & 1u) continue;
      load_words(e.words, out);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (e.version.load(std::memory_order_relaxed) == v1) return true;
    }
    return false;
  }

 private:
  const SegmentHeader* hdr_{nullptr};
  const SnapshotEntry* entries_{nullptr};
};

}  // namespace qm::shm
