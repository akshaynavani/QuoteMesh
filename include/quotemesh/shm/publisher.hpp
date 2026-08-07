#pragma once

#include <memory>
#include <string>
#include <vector>

#include "quotemesh/aggregator.hpp"
#include "quotemesh/shm/broadcast_ring.hpp"
#include "quotemesh/shm/shm_region.hpp"
#include "quotemesh/shm/snapshot_table.hpp"
#include "quotemesh/symbol_table.hpp"

namespace qm::shm {

struct PublisherConfig {
  std::string name{"/quotemesh"};
  std::uint32_t ring_capacity{1u << 16};
  std::uint32_t max_symbols{4096};
  bool lock_pages{false};
  bool unlink_on_close{true};
};

// Aggregator-side EventSink that distributes NBBO changes and trades to other processes:
// every event goes into the broadcast ring and the per-symbol snapshot is refreshed.
class ShmPublisher final : public EventSink {
 public:
  ShmPublisher(Geometry geo, ShmRegion region, const SymbolTable& symbols,
               const std::vector<std::string>& exchange_names);

  static std::unique_ptr<ShmPublisher> create(const PublisherConfig& cfg,
                                              const SymbolTable& symbols,
                                              const std::vector<std::string>& exchange_names);

  void on_nbbo(SymbolId sym, const Nbbo& nbbo, const MarketEvent& cause) override;
  void on_trade(SymbolId sym, const LastTrade& trade, const MarketEvent& cause) override;
  void on_venue_quote(SymbolId sym, const MarketEvent& ev) override;
  void on_idle() override;

  std::uint64_t published() const noexcept { return ring_.last_published(); }
  const ShmRegion& region() const noexcept { return region_; }
  const void* base() const noexcept { return region_.data(); }

 private:
  SnapshotData& cached(SymbolId sym);
  void heartbeat(Timestamp now);

  ShmRegion region_;
  const SymbolTable& symbols_;
  SegmentHeader* hdr_;
  RingWriter ring_;
  SnapshotWriter snaps_;
  std::vector<SnapshotData> cache_;  // private copy so updates never read back from shm
  Timestamp last_heartbeat_{0};
};

}  // namespace qm::shm
