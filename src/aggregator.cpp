#include "quotemesh/aggregator.hpp"

#include <chrono>

#include "quotemesh/clock.hpp"

namespace qm {

Aggregator::Aggregator(AggregatorConfig cfg, std::vector<FeedHandler*> feeds, EventSink& sink)
    : cfg_(cfg), feeds_(std::move(feeds)), sink_(sink), scratch_(cfg_.drain_batch) {}

Aggregator::~Aggregator() { stop(); }

void Aggregator::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void Aggregator::stop() {
  running_.store(false, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
}

void Aggregator::handle(const MarketEvent& ev) {
  stats_.events.fetch_add(1, std::memory_order_relaxed);
  if (ev.type == EventType::Quote) {
    stats_.quotes.fetch_add(1, std::memory_order_relaxed);
    if (cfg_.forward_venue_quotes) sink_.on_venue_quote(ev.symbol, ev);
    if (book_.apply_quote(ev.symbol, ev.exchange, ev.quote, ev.exch_ts)) {
      const Nbbo& n = *book_.nbbo(ev.symbol);
      stats_.nbbo_changes.fetch_add(1, std::memory_order_relaxed);
      if (n.crossed()) stats_.crossed.fetch_add(1, std::memory_order_relaxed);
      sink_.on_nbbo(ev.symbol, n, ev);
    }
  } else {
    stats_.trades.fetch_add(1, std::memory_order_relaxed);
    book_.apply_trade(ev.symbol, ev.exchange, ev.trade, ev.exch_ts);
    sink_.on_trade(ev.symbol, book_.book(ev.symbol)->last, ev);
  }
}

std::size_t Aggregator::poll_once() {
  std::size_t total = 0;
  for (FeedHandler* f : feeds_) {
    const std::size_t n = f->queue().pop_batch(scratch_.data(), scratch_.size());
    if (n == 0) continue;
    const Timestamp now = mono_ns();
    for (std::size_t i = 0; i < n; ++i) {
      stats_.feed_to_book_ns.record(now > scratch_[i].recv_ts ? now - scratch_[i].recv_ts : 0);
      handle(scratch_[i]);
    }
    total += n;
  }
  return total;
}

void Aggregator::run() {
  pin_current_thread(cfg_.cpu_affinity);
  while (running_.load(std::memory_order_acquire)) {
    if (poll_once() == 0) {
      sink_.on_idle();
      if (!cfg_.busy_poll) std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  }
  while (poll_once() != 0) {
  }  // final drain
}

}  // namespace qm
