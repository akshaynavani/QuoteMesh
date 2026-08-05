#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>

#include "quotemesh/types.hpp"

// Binary layout of the QuoteMesh shared-memory segment. Shared by the publisher (aggregator)
// and every subscriber process, so everything here is fixed-size and trivially copyable.
//
//   +-------------------+  offset 0
//   | SegmentHeader     |  magic, version, geometry, writer cursor, heartbeat
//   +-------------------+  header.ring_offset
//   | RingSlot[cap]     |  broadcast ring of 64-byte messages (NBBO / trade / venue quote)
//   +-------------------+  header.snapshot_offset
//   | SnapshotEntry[n]  |  latest NBBO + last trade per symbol, seqlock protected
//   +-------------------+
namespace qm::shm {

inline constexpr std::uint64_t kMagic = 0x48534D45544F5551ull;  // "QUOTEMSH" little-endian
inline constexpr std::uint32_t kVersion = 1;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free to be shared across processes");

enum class MsgType : std::uint8_t { Nbbo = 1, Trade = 2, VenueQuote = 3 };

enum MsgFlags : std::uint16_t {
  kFlagLocked = 1u << 0,
  kFlagCrossed = 1u << 1,
};

// Message body as seen by readers. For trades: bid_px/bid_sz hold price/size and
// ask_sz carries the low 32 bits of cumulative volume.
struct Payload {
  MsgType type;
  std::uint8_t exchange;
  std::uint16_t flags;
  SymbolId symbol;
  Timestamp exch_ts;
  Timestamp publish_ts;
  Price bid_px;
  Price ask_px;
  Quantity bid_sz;
  Quantity ask_sz;
  std::uint32_t bid_venues;
  std::uint32_t ask_venues;
};
static_assert(std::is_trivially_copyable_v<Payload>);
static_assert(sizeof(Payload) == 56);

inline constexpr std::size_t kPayloadWords = sizeof(Payload) / 8;

// One ring slot is exactly one cache line: an 8-byte sequence plus the 56-byte payload.
// Payload bytes are stored as relaxed atomics so concurrent seqlock reads are race-free.
struct alignas(kCacheLine) RingSlot {
  std::atomic<std::uint64_t> seq;  // message number stored here; 0 while being rewritten
  std::atomic<std::uint64_t> words[kPayloadWords];
};
static_assert(sizeof(RingSlot) == kCacheLine);

struct SnapshotData {
  char symbol[kMaxSymbolLen];
  Price bid_px;
  Price ask_px;
  Quantity bid_sz;
  Quantity ask_sz;
  std::uint32_t bid_venues;
  std::uint32_t ask_venues;
  Price last_px;
  Quantity last_sz;
  std::uint16_t last_exchange;
  std::uint16_t flags;
  std::uint64_t volume;
  Timestamp updated_ts;
};
static_assert(std::is_trivially_copyable_v<SnapshotData>);
static_assert(sizeof(SnapshotData) % 8 == 0);

inline constexpr std::size_t kSnapshotWords = sizeof(SnapshotData) / 8;

struct alignas(kCacheLine) SnapshotEntry {
  std::atomic<std::uint64_t> version;  // seqlock: odd while the writer is mid-update, 0 = unused
  std::atomic<std::uint64_t> words[kSnapshotWords];
};

struct alignas(kCacheLine) SegmentHeader {
  std::uint64_t magic;
  std::uint32_t version;
  std::uint32_t ring_capacity;  // power of two
  std::uint32_t max_symbols;
  std::uint32_t max_exchanges;
  std::uint64_t ring_offset;
  std::uint64_t snapshot_offset;
  std::uint64_t total_size;
  std::int64_t writer_pid;
  char exchange_names[kMaxExchanges][8];

  alignas(kCacheLine) std::atomic<std::uint64_t> write_seq;     // last published message number
  std::atomic<std::uint64_t> heartbeat_ns;                       // wall clock, updated when idle
  std::atomic<std::uint32_t> symbol_count;                       // high-water mark of symbol ids
  std::atomic<std::uint32_t> ready;                              // 1 once initialised
};

struct Geometry {
  std::uint32_t ring_capacity;
  std::uint32_t max_symbols;

  static constexpr std::size_t align_up(std::size_t v) noexcept {
    return (v + kCacheLine - 1) & ~(kCacheLine - 1);
  }
  std::size_t ring_offset() const noexcept { return align_up(sizeof(SegmentHeader)); }
  std::size_t snapshot_offset() const noexcept {
    return align_up(ring_offset() + static_cast<std::size_t>(ring_capacity) * sizeof(RingSlot));
  }
  std::size_t total_size() const noexcept {
    return align_up(snapshot_offset() +
                    static_cast<std::size_t>(max_symbols) * sizeof(SnapshotEntry));
  }
  void validate() const {
    if (ring_capacity < 2 || (ring_capacity & (ring_capacity - 1)) != 0)
      throw std::invalid_argument("ring capacity must be a power of two >= 2");
    if (max_symbols == 0) throw std::invalid_argument("max_symbols must be > 0");
  }
};

// Copy helpers between plain structs and relaxed-atomic word arrays.
template <typename T, std::size_t N>
inline void store_words(std::atomic<std::uint64_t> (&dst)[N], const T& src) noexcept {
  static_assert(sizeof(T) == N * 8);
  std::uint64_t tmp[N];
  std::memcpy(tmp, &src, sizeof(T));
  for (std::size_t i = 0; i < N; ++i) dst[i].store(tmp[i], std::memory_order_relaxed);
}

template <typename T, std::size_t N>
inline void load_words(const std::atomic<std::uint64_t> (&src)[N], T& dst) noexcept {
  static_assert(sizeof(T) == N * 8);
  std::uint64_t tmp[N];
  for (std::size_t i = 0; i < N; ++i) tmp[i] = src[i].load(std::memory_order_relaxed);
  std::memcpy(&dst, tmp, sizeof(T));
}

}  // namespace qm::shm
