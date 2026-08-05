#pragma once

#include <atomic>
#include <cstdint>

#include "quotemesh/shm/layout.hpp"

namespace qm::shm {

// Single-producer / multi-consumer broadcast ring living in shared memory.
//
// The writer never waits for readers: message n overwrites slot (n & mask). Each slot holds a
// per-slot sequence number acting as a seqlock, so a reader can tell whether the slot still
// contains the message it wants, has not been written yet, or was already overwritten
// (the reader fell more than `capacity` messages behind).
class RingWriter {
 public:
  RingWriter() = default;
  RingWriter(SegmentHeader* hdr, RingSlot* slots) noexcept
      : hdr_(hdr), slots_(slots), mask_(hdr->ring_capacity - 1),
        next_(hdr->write_seq.load(std::memory_order_relaxed) + 1) {}

  // Returns the message number assigned to `p`.
  std::uint64_t publish(const Payload& p) noexcept {
    const std::uint64_t n = next_++;
    RingSlot& s = slots_[n & mask_];
    s.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    store_words(s.words, p);
    s.seq.store(n, std::memory_order_release);
    hdr_->write_seq.store(n, std::memory_order_release);
    return n;
  }

  std::uint64_t last_published() const noexcept { return next_ - 1; }

 private:
  SegmentHeader* hdr_{nullptr};
  RingSlot* slots_{nullptr};
  std::uint64_t mask_{0};
  std::uint64_t next_{1};
};

enum class ReadStatus { Ok, Empty, Overrun };

class RingReader {
 public:
  RingReader() = default;
  // `from_latest`: start at the next message to be written instead of the oldest retained one.
  RingReader(const SegmentHeader* hdr, const RingSlot* slots, bool from_latest = true) noexcept
      : hdr_(hdr), slots_(slots), cap_(hdr->ring_capacity), mask_(hdr->ring_capacity - 1) {
    const std::uint64_t w = hdr_->write_seq.load(std::memory_order_acquire);
    next_ = from_latest ? w + 1 : (w >= cap_ ? w - cap_ + 1 : 1);
  }

  // Try to read the next message. On Overrun the reader resynchronises to the oldest message
  // still retained and `lost` reports how many messages were skipped.
  ReadStatus try_read(Payload& out) noexcept {
    const RingSlot& s = slots_[next_ & mask_];
    const std::uint64_t s1 = s.seq.load(std::memory_order_acquire);
    if (s1 == next_) {
      load_words(s.words, out);
      std::atomic_thread_fence(std::memory_order_acquire);
      const std::uint64_t s2 = s.seq.load(std::memory_order_relaxed);
      if (s2 == next_) {
        ++next_;
        return ReadStatus::Ok;
      }
    } else if (s1 < next_ && s1 != 0) {
      return ReadStatus::Empty;  // writer has not reached this slot yet
    } else if (s1 == 0 && hdr_->write_seq.load(std::memory_order_acquire) < next_) {
      return ReadStatus::Empty;  // never-written slot, or rewrite of an older lap in progress
    }
    resync();
    return ReadStatus::Overrun;
  }

  std::uint64_t next_seq() const noexcept { return next_; }
  std::uint64_t lost() const noexcept { return lost_; }
  std::uint64_t lag() const noexcept {
    const std::uint64_t w = hdr_->write_seq.load(std::memory_order_acquire);
    return w >= next_ ? w - next_ + 1 : 0;
  }

 private:
  void resync() noexcept {
    const std::uint64_t w = hdr_->write_seq.load(std::memory_order_acquire);
    // Jump to half a ring behind the writer to leave headroom before being lapped again.
    const std::uint64_t target = w > cap_ / 2 ? w - cap_ / 2 + 1 : 1;
    if (target > next_) {
      lost_ += target - next_;
      next_ = target;
    }
  }

  const SegmentHeader* hdr_{nullptr};
  const RingSlot* slots_{nullptr};
  std::uint64_t cap_{0};
  std::uint64_t mask_{0};
  std::uint64_t next_{1};
  std::uint64_t lost_{0};
};

}  // namespace qm::shm
