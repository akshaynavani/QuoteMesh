#include "quotemesh/shm/subscriber.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace qm::shm {

ShmSubscriber ShmSubscriber::open(const std::string& name, bool from_latest) {
  return ShmSubscriber(ShmRegion::open(name, /*read_only=*/true), from_latest);
}

ShmSubscriber::ShmSubscriber(const void* base, bool from_latest) { attach(base, from_latest); }

ShmSubscriber::ShmSubscriber(ShmRegion region, bool from_latest) : region_(std::move(region)) {
  if (region_.size() < sizeof(SegmentHeader)) throw std::runtime_error("shm segment too small");
  attach(region_.data(), from_latest);
  if (region_.size() < hdr_->total_size) throw std::runtime_error("shm segment truncated");
}

void ShmSubscriber::attach(const void* base, bool from_latest) {
  const auto* hdr = static_cast<const SegmentHeader*>(base);
  // The publisher may still be initialising right after creating the segment.
  for (int i = 0; i < 200 && hdr->ready.load(std::memory_order_acquire) == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (hdr->ready.load(std::memory_order_acquire) == 0)
    throw std::runtime_error("shm segment never became ready");
  if (hdr->magic != kMagic) throw std::runtime_error("not a QuoteMesh shm segment");
  if (hdr->version != kVersion)
    throw std::runtime_error("shm layout version mismatch: " + std::to_string(hdr->version));

  const auto* bytes = static_cast<const char*>(base);
  hdr_ = hdr;
  ring_ = RingReader(hdr, reinterpret_cast<const RingSlot*>(bytes + hdr->ring_offset), from_latest);
  snaps_ = SnapshotReader(hdr, reinterpret_cast<const SnapshotEntry*>(bytes + hdr->snapshot_offset));
}

std::string ShmSubscriber::symbol_name(SymbolId sym) const {
  SnapshotData d{};
  if (!snaps_.read(sym, d)) return {};
  return std::string(d.symbol, strnlen(d.symbol, kMaxSymbolLen));
}

std::optional<SymbolId> ShmSubscriber::find_symbol(const std::string& name) const {
  const std::uint32_t n = symbol_count();
  for (SymbolId s = 0; s < n; ++s) {
    if (symbol_name(s) == name) return s;
  }
  return std::nullopt;
}

std::string ShmSubscriber::exchange_name(std::uint8_t ex) const {
  if (ex >= kMaxExchanges) return "?";
  const char* n = hdr_->exchange_names[ex];
  return std::string(n, strnlen(n, 8));
}

bool ShmSubscriber::writer_alive(Timestamp now_wall_ns, Timestamp timeout_ns) const noexcept {
  const Timestamp hb = hdr_->heartbeat_ns.load(std::memory_order_acquire);
  return hb != 0 && now_wall_ns >= hb && now_wall_ns - hb < timeout_ns;
}

}  // namespace qm::shm
