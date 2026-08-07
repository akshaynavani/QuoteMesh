#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <unistd.h>

#include "quotemesh/clock.hpp"
#include "quotemesh/shm/publisher.hpp"
#include "quotemesh/shm/subscriber.hpp"

using namespace qm;
using namespace qm::shm;

namespace {

std::unique_ptr<ShmPublisher> make_anon_publisher(SymbolTable& symbols, std::uint32_t cap,
                                                  std::uint32_t max_symbols = 64) {
  Geometry geo{cap, max_symbols};
  return std::make_unique<ShmPublisher>(geo, ShmRegion::anonymous(geo.total_size()), symbols,
                                        std::vector<std::string>{"NYSE", "NASDAQ", "ARCA"});
}

MarketEvent quote_event(ExchangeId ex, SymbolId sym) {
  MarketEvent ev;
  ev.exchange = ex;
  ev.symbol = sym;
  ev.exch_ts = 123;
  return ev;
}

}  // namespace

TEST(ShmLayout, GeometryIsCacheAligned) {
  Geometry g{1024, 100};
  EXPECT_EQ(g.ring_offset() % kCacheLine, 0u);
  EXPECT_EQ(g.snapshot_offset() % kCacheLine, 0u);
  EXPECT_GE(g.total_size(), g.snapshot_offset() + 100 * sizeof(SnapshotEntry));
  EXPECT_THROW((Geometry{1000, 10}.validate()), std::invalid_argument);
}

TEST(ShmRing, SubscriberReceivesNbboAndTrades) {
  SymbolTable symbols;
  const SymbolId aapl = symbols.intern("AAPL");
  auto pub = make_anon_publisher(symbols, 64);
  ShmSubscriber sub(pub->base());

  Nbbo n;
  n.bid_px = 1'000'000; n.ask_px = 1'000'100; n.bid_sz = 300; n.ask_sz = 200;
  n.bid_venues = 0b101; n.ask_venues = 0b010;
  pub->on_nbbo(aapl, n, quote_event(1, aapl));
  LastTrade t{1'000'050, 75, 2, 9, 75};
  pub->on_trade(aapl, t, quote_event(2, aapl));

  Payload p{};
  ASSERT_EQ(sub.poll(p), ReadStatus::Ok);
  EXPECT_EQ(p.type, MsgType::Nbbo);
  EXPECT_EQ(p.bid_px, 1'000'000);
  EXPECT_EQ(p.bid_venues, 0b101u);
  ASSERT_EQ(sub.poll(p), ReadStatus::Ok);
  EXPECT_EQ(p.type, MsgType::Trade);
  EXPECT_EQ(p.bid_px, 1'000'050);
  EXPECT_EQ(sub.poll(p), ReadStatus::Empty);

  SnapshotData d{};
  ASSERT_TRUE(sub.snapshot(aapl, d));
  EXPECT_EQ(sub.symbol_name(aapl), "AAPL");
  EXPECT_EQ(d.ask_px, 1'000'100);
  EXPECT_EQ(d.last_px, 1'000'050);
  EXPECT_EQ(d.volume, 75u);
  EXPECT_EQ(sub.exchange_name(1), "NASDAQ");
  EXPECT_EQ(sub.find_symbol("AAPL"), aapl);
  EXPECT_TRUE(sub.writer_alive(wall_ns()));
}

TEST(ShmRing, SlowReaderDetectsOverrun) {
  SymbolTable symbols;
  const SymbolId s = symbols.intern("SPY");
  auto pub = make_anon_publisher(symbols, 16);
  ShmSubscriber sub(pub->base());

  Nbbo n;
  n.bid_sz = n.ask_sz = 1;
  for (int i = 0; i < 100; ++i) {
    n.bid_px = 1000 + i;
    n.ask_px = 2000 + i;
    pub->on_nbbo(s, n, quote_event(0, s));
  }
  Payload p{};
  EXPECT_EQ(sub.poll(p), ReadStatus::Overrun);
  EXPECT_GT(sub.lost(), 0u);
  // After resync the reader gets a contiguous tail of the stream ending at the latest message.
  Price last = 0;
  int got = 0;
  while (sub.poll(p) == ReadStatus::Ok) {
    if (got > 0) EXPECT_EQ(p.bid_px, last + 1);
    last = p.bid_px;
    ++got;
  }
  EXPECT_EQ(last, 1099);
  EXPECT_GT(got, 0);
}

TEST(ShmRing, ConcurrentReaderSeesConsistentMessages) {
  SymbolTable symbols;
  const SymbolId s = symbols.intern("QQQ");
  auto pub = make_anon_publisher(symbols, 1024);
  ShmSubscriber sub(pub->base());
  constexpr int kCount = 200'000;

  std::thread writer([&] {
    Nbbo n;
    for (int i = 1; i <= kCount; ++i) {
      // Invariant checked by the reader: ask = 2 * bid, sizes = bid.
      n.bid_px = i;
      n.ask_px = 2 * i;
      n.bid_sz = n.ask_sz = static_cast<Quantity>(i);
      pub->on_nbbo(s, n, quote_event(0, s));
    }
  });

  Payload p{};
  int received = 0;
  Price last = 0;
  while (last < kCount) {
    const auto st = sub.poll(p);
    if (st != ReadStatus::Ok) continue;
    ASSERT_EQ(p.ask_px, 2 * p.bid_px) << "torn read";
    ASSERT_EQ(p.bid_sz, static_cast<Quantity>(p.bid_px));
    ASSERT_GT(p.bid_px, last);
    last = p.bid_px;
    ++received;
  }
  writer.join();
  EXPECT_EQ(static_cast<std::uint64_t>(received) + sub.lost(), static_cast<std::uint64_t>(kCount));
}

TEST(ShmRegion, NamedSegmentRoundTrip) {
  const std::string name = "/qm_test_" + std::to_string(::getpid());
  SymbolTable symbols;
  const SymbolId s = symbols.intern("NVDA");
  PublisherConfig cfg;
  cfg.name = name;
  cfg.ring_capacity = 64;
  cfg.max_symbols = 16;
  auto pub = ShmPublisher::create(cfg, symbols, {"X"});
  auto sub = ShmSubscriber::open(name);
  Nbbo n;
  n.bid_px = 5; n.ask_px = 6; n.bid_sz = n.ask_sz = 1;
  pub->on_nbbo(s, n, quote_event(0, s));
  Payload p{};
  ASSERT_EQ(sub.poll(p), ReadStatus::Ok);
  EXPECT_EQ(p.ask_px, 6);
}
