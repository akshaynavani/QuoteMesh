#include "quotemesh/shm/publisher.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <unistd.h>

#include "quotemesh/clock.hpp"

namespace qm::shm {
namespace {

std::uint16_t nbbo_flags(const Nbbo& n) {
  std::uint16_t f = 0;
  if (n.locked()) f |= kFlagLocked;
  if (n.crossed()) f |= kFlagCrossed;
  return f;
}

}  // namespace

ShmPublisher::ShmPublisher(Geometry geo, ShmRegion region, const SymbolTable& symbols,
                           const std::vector<std::string>& exchange_names)
    : region_(std::move(region)), symbols_(symbols) {
  geo.validate();
  if (region_.size() < geo.total_size()) throw std::invalid_argument("shm region too small");

  auto* base = static_cast<char*>(region_.data());
  hdr_ = new (base) SegmentHeader{};
  hdr_->magic = kMagic;
  hdr_->version = kVersion;
  hdr_->ring_capacity = geo.ring_capacity;
  hdr_->max_symbols = geo.max_symbols;
  hdr_->max_exchanges = static_cast<std::uint32_t>(kMaxExchanges);
  hdr_->ring_offset = geo.ring_offset();
  hdr_->snapshot_offset = geo.snapshot_offset();
  hdr_->total_size = geo.total_size();
  hdr_->writer_pid = static_cast<std::int64_t>(::getpid());
  for (std::size_t i = 0; i < exchange_names.size() && i < kMaxExchanges; ++i) {
    std::strncpy(hdr_->exchange_names[i], exchange_names[i].c_str(), 7);
  }

  auto* slots = reinterpret_cast<RingSlot*>(base + hdr_->ring_offset);
  for (std::uint32_t i = 0; i < geo.ring_capacity; ++i) new (&slots[i]) RingSlot{};
  auto* entries = reinterpret_cast<SnapshotEntry*>(base + hdr_->snapshot_offset);
  for (std::uint32_t i = 0; i < geo.max_symbols; ++i) new (&entries[i]) SnapshotEntry{};

  ring_ = RingWriter(hdr_, slots);
  snaps_ = SnapshotWriter(hdr_, entries);
  heartbeat(wall_ns());
  hdr_->ready.store(1, std::memory_order_release);
}

std::unique_ptr<ShmPublisher> ShmPublisher::create(const PublisherConfig& cfg,
                                                   const SymbolTable& symbols,
                                                   const std::vector<std::string>& exchanges) {
  Geometry geo{cfg.ring_capacity, cfg.max_symbols};
  geo.validate();
  ShmRegion region = ShmRegion::create(cfg.name, geo.total_size(), cfg.unlink_on_close);
  if (cfg.lock_pages) region.lock_pages();
  return std::make_unique<ShmPublisher>(geo, std::move(region), symbols, exchanges);
}

SnapshotData& ShmPublisher::cached(SymbolId sym) {
  if (sym >= cache_.size()) cache_.resize(static_cast<std::size_t>(sym) + 1, SnapshotData{});
  SnapshotData& d = cache_[sym];
  if (d.symbol[0] == '\0') {
    const std::string name = symbols_.name(sym);
    std::memcpy(d.symbol, name.data(), std::min(name.size(), kMaxSymbolLen));
    d.bid_px = kNoBid;
    d.ask_px = kNoAsk;
  }
  return d;
}

void ShmPublisher::on_nbbo(SymbolId sym, const Nbbo& n, const MarketEvent& cause) {
  const Timestamp now = wall_ns();
  Payload p{};
  p.type = MsgType::Nbbo;
  p.exchange = static_cast<std::uint8_t>(cause.exchange);
  p.flags = nbbo_flags(n);
  p.symbol = sym;
  p.exch_ts = cause.exch_ts;
  p.publish_ts = now;
  p.bid_px = n.bid_px;
  p.ask_px = n.ask_px;
  p.bid_sz = n.bid_sz;
  p.ask_sz = n.ask_sz;
  p.bid_venues = n.bid_venues;
  p.ask_venues = n.ask_venues;
  ring_.publish(p);

  SnapshotData& d = cached(sym);
  d.bid_px = n.bid_px;
  d.ask_px = n.ask_px;
  d.bid_sz = n.bid_sz;
  d.ask_sz = n.ask_sz;
  d.bid_venues = n.bid_venues;
  d.ask_venues = n.ask_venues;
  d.flags = p.flags;
  d.updated_ts = now;
  snaps_.write(sym, d);
}

void ShmPublisher::on_trade(SymbolId sym, const LastTrade& t, const MarketEvent& cause) {
  const Timestamp now = wall_ns();
  Payload p{};
  p.type = MsgType::Trade;
  p.exchange = static_cast<std::uint8_t>(t.exchange);
  p.symbol = sym;
  p.exch_ts = cause.exch_ts;
  p.publish_ts = now;
  p.bid_px = t.px;
  p.bid_sz = t.sz;
  p.ask_px = 0;
  p.ask_sz = static_cast<Quantity>(t.volume & 0xFFFFFFFFu);
  ring_.publish(p);

  SnapshotData& d = cached(sym);
  d.last_px = t.px;
  d.last_sz = t.sz;
  d.last_exchange = t.exchange;
  d.volume = t.volume;
  d.updated_ts = now;
  snaps_.write(sym, d);
}

void ShmPublisher::on_venue_quote(SymbolId sym, const MarketEvent& ev) {
  Payload p{};
  p.type = MsgType::VenueQuote;
  p.exchange = static_cast<std::uint8_t>(ev.exchange);
  p.symbol = sym;
  p.exch_ts = ev.exch_ts;
  p.publish_ts = wall_ns();
  p.bid_px = ev.quote.bid_px;
  p.ask_px = ev.quote.ask_px;
  p.bid_sz = ev.quote.bid_sz;
  p.ask_sz = ev.quote.ask_sz;
  ring_.publish(p);
}

void ShmPublisher::heartbeat(Timestamp now) {
  last_heartbeat_ = now;
  hdr_->heartbeat_ns.store(now, std::memory_order_release);
}

void ShmPublisher::on_idle() {
  const Timestamp now = wall_ns();
  if (now - last_heartbeat_ > 100'000'000) heartbeat(now);  // every 100ms when idle
}

}  // namespace qm::shm
