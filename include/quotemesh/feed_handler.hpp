#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "quotemesh/feed_source.hpp"
#include "quotemesh/parser.hpp"
#include "quotemesh/spsc_queue.hpp"
#include "quotemesh/stats.hpp"
#include "quotemesh/symbol_table.hpp"

namespace qm {

struct FeedHandlerConfig {
  ExchangeId exchange{0};
  std::string name{"EX"};
  std::size_t queue_capacity{1 << 16};
  std::size_t read_buffer_bytes{1 << 16};
  bool use_simd{true};
  int cpu_affinity{-1};      // best effort; -1 = don't pin
  bool busy_poll{true};      // spin when idle instead of sleeping
};

// One thread per exchange: read raw bytes -> SIMD parse -> sequence check -> normalise ->
// push MarketEvents into a dedicated SPSC queue consumed by the aggregator.
class FeedHandler {
 public:
  FeedHandler(FeedHandlerConfig cfg, std::unique_ptr<FeedSource> source, SymbolTable& symbols);
  ~FeedHandler();

  FeedHandler(const FeedHandler&) = delete;
  FeedHandler& operator=(const FeedHandler&) = delete;

  void start();
  void stop();
  bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }

  // Process whatever the source currently has; returns number of events published.
  // Exposed for deterministic single-threaded tests; normally driven by run().
  std::size_t poll_once();

  SpscQueue<MarketEvent>& queue() noexcept { return queue_; }
  const FeedStats& stats() const noexcept { return stats_; }
  const FeedHandlerConfig& config() const noexcept { return cfg_; }
  std::string describe() const { return cfg_.name + " <- " + source_->describe(); }

 private:
  void run();
  SymbolId resolve(const ParsedMessage& m);
  void publish(const MarketEvent& ev);

  FeedHandlerConfig cfg_;
  std::unique_ptr<FeedSource> source_;
  SymbolTable& symbols_;
  SpscQueue<MarketEvent> queue_;
  FeedStats stats_;

  std::vector<char> buf_;
  std::size_t buf_len_{0};
  std::vector<ParsedMessage> parsed_;
  std::unordered_map<std::uint64_t, SymbolId> sym_cache_;  // packed 8-byte symbol -> id
  SeqNum last_seq_{0};

  std::atomic<bool> running_{false};
  std::atomic<bool> finished_{false};
  std::thread thread_;
};

// Best-effort CPU pinning (Linux only; no-op elsewhere).
void pin_current_thread(int cpu);

}  // namespace qm
