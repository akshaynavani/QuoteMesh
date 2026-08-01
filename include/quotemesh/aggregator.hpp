#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include "quotemesh/consolidated_book.hpp"
#include "quotemesh/feed_handler.hpp"
#include "quotemesh/stats.hpp"

namespace qm {

// Downstream consumer of aggregated output (the shared-memory publisher in production,
// a recording sink in tests). Called only from the aggregator thread.
class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void on_nbbo(SymbolId sym, const Nbbo& nbbo, const MarketEvent& cause) = 0;
  virtual void on_trade(SymbolId sym, const LastTrade& trade, const MarketEvent& cause) = 0;
  virtual void on_venue_quote(SymbolId, const MarketEvent&) {}
  virtual void on_idle() {}
};

struct AggregatorConfig {
  std::size_t drain_batch{256};  // max events taken from one feed per round-robin turn
  int cpu_affinity{-1};
  bool busy_poll{true};
  bool forward_venue_quotes{false};
};

// Single aggregator thread: fair round-robin drain of every feed handler's SPSC queue,
// apply to the consolidated book, forward NBBO changes and trades to the sink.
class Aggregator {
 public:
  Aggregator(AggregatorConfig cfg, std::vector<FeedHandler*> feeds, EventSink& sink);
  ~Aggregator();

  Aggregator(const Aggregator&) = delete;
  Aggregator& operator=(const Aggregator&) = delete;

  void start();
  void stop();  // drains remaining events before returning

  // One round-robin pass over all feeds; returns events processed. Used by tests and run().
  std::size_t poll_once();

  const ConsolidatedBook& book() const noexcept { return book_; }
  const AggregatorStats& stats() const noexcept { return stats_; }

 private:
  void run();
  void handle(const MarketEvent& ev);

  AggregatorConfig cfg_;
  std::vector<FeedHandler*> feeds_;
  EventSink& sink_;
  ConsolidatedBook book_;
  AggregatorStats stats_;
  std::vector<MarketEvent> scratch_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace qm
