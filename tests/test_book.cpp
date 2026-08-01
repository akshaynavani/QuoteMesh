#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "quotemesh/aggregator.hpp"
#include "quotemesh/consolidated_book.hpp"
#include "quotemesh/feed_handler.hpp"
#include "quotemesh/synthetic_feed.hpp"
#include "quotemesh/wire_format.hpp"

using namespace qm;

TEST(ConsolidatedBook, BuildsNbboAcrossVenues) {
  ConsolidatedBook book;
  EXPECT_TRUE(book.apply_quote(0, 0, Quote{1000, 1010, 100, 100}, 1));
  EXPECT_TRUE(book.apply_quote(0, 1, Quote{1002, 1012, 50, 70}, 2));
  const Nbbo* n = book.nbbo(0);
  EXPECT_EQ(n->bid_px, 1002);
  EXPECT_EQ(n->bid_sz, 50u);
  EXPECT_EQ(n->bid_venues, 0b10u);
  EXPECT_EQ(n->ask_px, 1010);
  EXPECT_EQ(n->ask_venues, 0b01u);
}

TEST(ConsolidatedBook, AggregatesSizeAtSamePrice) {
  ConsolidatedBook book;
  book.apply_quote(0, 0, Quote{1000, 1010, 100, 100}, 1);
  EXPECT_TRUE(book.apply_quote(0, 3, Quote{1000, 1010, 25, 30}, 2));
  const Nbbo* n = book.nbbo(0);
  EXPECT_EQ(n->bid_sz, 125u);
  EXPECT_EQ(n->ask_sz, 130u);
  EXPECT_EQ(n->bid_venues, 0b1001u);
}

TEST(ConsolidatedBook, IgnoresQuotesBehindTheMarket) {
  ConsolidatedBook book;
  book.apply_quote(0, 0, Quote{1000, 1010, 100, 100}, 1);
  EXPECT_FALSE(book.apply_quote(0, 1, Quote{990, 1020, 100, 100}, 2));
  EXPECT_FALSE(book.apply_quote(0, 1, Quote{995, 1015, 100, 100}, 3));
}

TEST(ConsolidatedBook, FallsBackWhenBestVenueBacksOff) {
  ConsolidatedBook book;
  book.apply_quote(0, 0, Quote{1000, 1010, 100, 100}, 1);
  book.apply_quote(0, 1, Quote{999, 1011, 100, 100}, 2);
  EXPECT_TRUE(book.apply_quote(0, 0, Quote{kNoBid, kNoAsk, 0, 0}, 3));
  EXPECT_EQ(book.nbbo(0)->bid_px, 999);
  EXPECT_EQ(book.nbbo(0)->ask_px, 1011);
}

TEST(ConsolidatedBook, DetectsCrossedMarket) {
  ConsolidatedBook book;
  book.apply_quote(0, 0, Quote{1000, 1010, 1, 1}, 1);
  book.apply_quote(0, 1, Quote{1012, 1020, 1, 1}, 2);
  EXPECT_TRUE(book.nbbo(0)->crossed());
}

TEST(ConsolidatedBook, ClearVenueRemovesItsQuotes) {
  ConsolidatedBook book;
  book.apply_quote(0, 2, Quote{1000, 1010, 1, 1}, 1);
  book.apply_quote(1, 2, Quote{2000, 2010, 1, 1}, 1);
  auto changed = book.clear_venue(2, 5);
  EXPECT_EQ(changed.size(), 2u);
  EXPECT_FALSE(book.nbbo(0)->has_bid());
}

namespace {

// Feed source that replays a fixed string once.
class StringFeed : public FeedSource {
 public:
  explicit StringFeed(std::string data) : data_(std::move(data)) {}
  ssize_t read(char* buf, std::size_t cap) override {
    if (pos_ >= data_.size()) return -1;
    const std::size_t n = std::min(cap, std::min<std::size_t>(37, data_.size() - pos_));
    std::memcpy(buf, data_.data() + pos_, n);  // deliberately split messages across reads
    pos_ += n;
    return static_cast<ssize_t>(n);
  }
  std::string describe() const override { return "string"; }

 private:
  std::string data_;
  std::size_t pos_{0};
};

struct RecordingSink : EventSink {
  std::vector<std::pair<SymbolId, Nbbo>> nbbos;
  std::vector<LastTrade> trades;
  void on_nbbo(SymbolId s, const Nbbo& n, const MarketEvent&) override { nbbos.emplace_back(s, n); }
  void on_trade(SymbolId, const LastTrade& t, const MarketEvent&) override { trades.push_back(t); }
};

}  // namespace

TEST(FeedHandler, ReassemblesSplitMessagesAndDetectsGaps) {
  SymbolTable symbols;
  const std::string data =
      "Q|1|AAPL|10|100|5|101|5\nQ|2|AAPL|11|100.5|5|101|5\nQ|5|MSFT|12|50|1|51|1\n"
      "Q|4|MSFT|13|50|1|51|1\nT|6|AAPL|14|100.75|10\n";
  FeedHandler fh({.exchange = 3, .name = "TEST"}, std::make_unique<StringFeed>(data), symbols);
  while (!fh.finished()) fh.poll_once();
  EXPECT_EQ(fh.stats().messages.load(), 5u);
  EXPECT_EQ(fh.stats().gaps.load(), 2u);   // seq 3 and 4 missing when 5 arrived
  EXPECT_EQ(fh.stats().stale.load(), 1u);  // seq 4 arrived late
  EXPECT_EQ(fh.queue().size_approx(), 4u);
  EXPECT_EQ(symbols.size(), 2u);
}

TEST(Aggregator, ProducesNbboFromMultipleFeeds) {
  SymbolTable symbols;
  FeedHandler a({.exchange = 0, .name = "A"},
                std::make_unique<StringFeed>("Q|1|AAPL|1|100|5|101|5\n"), symbols);
  FeedHandler b({.exchange = 1, .name = "B"},
                std::make_unique<StringFeed>("Q|1|AAPL|2|100.5|7|101.5|3\nT|2|AAPL|3|100.5|9\n"),
                symbols);
  while (!a.finished()) a.poll_once();
  while (!b.finished()) b.poll_once();

  RecordingSink sink;
  Aggregator agg({}, {&a, &b}, sink);
  while (agg.poll_once() != 0) {
  }
  ASSERT_FALSE(sink.nbbos.empty());
  const Nbbo& last = sink.nbbos.back().second;
  EXPECT_EQ(last.bid_px, 1'005'000);
  EXPECT_EQ(last.bid_venues, 0b10u);
  EXPECT_EQ(last.ask_px, 1'010'000);
  EXPECT_EQ(last.ask_venues, 0b01u);
  ASSERT_EQ(sink.trades.size(), 1u);
  EXPECT_EQ(sink.trades[0].volume, 9u);
}

TEST(Pipeline, ThreadedSyntheticFeedsReachAggregator) {
  SymbolTable symbols;
  std::vector<std::unique_ptr<FeedHandler>> handlers;
  std::vector<FeedHandler*> raw;
  for (ExchangeId ex = 0; ex < 4; ++ex) {
    SyntheticFeedConfig sc;
    sc.exchange_name = "EX" + std::to_string(ex);
    sc.symbols = {"AAPL", "MSFT", "SPY", "QQQ"};
    sc.messages_per_sec = 0;
    sc.max_messages = 20'000;
    sc.seed = ex + 1;
    handlers.push_back(std::make_unique<FeedHandler>(
        FeedHandlerConfig{.exchange = ex, .name = sc.exchange_name, .queue_capacity = 4096},
        std::make_unique<SyntheticFeed>(sc), symbols));
    raw.push_back(handlers.back().get());
  }
  RecordingSink sink;
  Aggregator agg({}, raw, sink);
  agg.start();
  for (auto& h : handlers) h->start();
  for (auto& h : handlers) {
    while (!h->finished()) std::this_thread::yield();
  }
  agg.stop();
  EXPECT_EQ(agg.stats().events.load(), 80'000u);
  EXPECT_GT(agg.stats().nbbo_changes.load(), 0u);
  EXPECT_EQ(symbols.size(), 4u);
}
