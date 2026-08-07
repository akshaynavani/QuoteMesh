#pragma once

#include <optional>
#include <string>

#include "quotemesh/shm/broadcast_ring.hpp"
#include "quotemesh/shm/shm_region.hpp"
#include "quotemesh/shm/snapshot_table.hpp"

namespace qm::shm {

// Downstream-process view of a QuoteMesh segment: a ring cursor for streaming updates plus
// random access to the snapshot table. Never writes to shared memory.
class ShmSubscriber {
 public:
  // Map the named segment read-only and validate its header.
  static ShmSubscriber open(const std::string& name, bool from_latest = true);

  // Attach to an already-mapped segment (non-owning; used by in-process tests and tools).
  explicit ShmSubscriber(const void* base, bool from_latest = true);

  ShmSubscriber(ShmSubscriber&&) noexcept = default;
  ShmSubscriber& operator=(ShmSubscriber&&) noexcept = default;

  ReadStatus poll(Payload& out) noexcept { return ring_.try_read(out); }
  bool snapshot(SymbolId sym, SnapshotData& out) const noexcept { return snaps_.read(sym, out); }
  std::uint32_t symbol_count() const noexcept { return snaps_.symbol_count(); }

  // Resolve a symbol id to its name via the snapshot table (empty if unknown).
  std::string symbol_name(SymbolId sym) const;
  std::optional<SymbolId> find_symbol(const std::string& name) const;
  std::string exchange_name(std::uint8_t ex) const;

  // True if the publisher refreshed its heartbeat within `timeout_ns`.
  bool writer_alive(Timestamp now_wall_ns, Timestamp timeout_ns = 2'000'000'000) const noexcept;

  std::uint64_t lost() const noexcept { return ring_.lost(); }
  std::uint64_t lag() const noexcept { return ring_.lag(); }
  const SegmentHeader& header() const noexcept { return *hdr_; }

 private:
  ShmSubscriber(ShmRegion region, bool from_latest);
  void attach(const void* base, bool from_latest);

  ShmRegion region_;
  const SegmentHeader* hdr_{nullptr};
  RingReader ring_;
  SnapshotReader snaps_;
};

}  // namespace qm::shm
