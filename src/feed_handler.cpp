#include "quotemesh/feed_handler.hpp"

#include <cstring>
#include <string_view>

#include "quotemesh/clock.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace qm {

void pin_current_thread(int cpu) {
#if defined(__linux__)
  if (cpu < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  pthread_setaffinity_np(pthread_self(), sizeof set, &set);
#else
  (void)cpu;
#endif
}

FeedHandler::FeedHandler(FeedHandlerConfig cfg, std::unique_ptr<FeedSource> source,
                         SymbolTable& symbols)
    : cfg_(std::move(cfg)),
      source_(std::move(source)),
      symbols_(symbols),
      queue_(cfg_.queue_capacity),
      buf_(cfg_.read_buffer_bytes + wire::kMaxMessageLen),
      parsed_(1024) {
  sym_cache_.reserve(1024);
}

FeedHandler::~FeedHandler() { stop(); }

void FeedHandler::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void FeedHandler::stop() {
  running_.store(false, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
}

SymbolId FeedHandler::resolve(const ParsedMessage& m) {
  std::uint64_t key = 0;
  std::memcpy(&key, m.symbol, kMaxSymbolLen);  // symbol is zero padded to 8 bytes
  if (auto it = sym_cache_.find(key); it != sym_cache_.end()) return it->second;
  const SymbolId id = symbols_.intern(std::string_view(m.symbol, m.symbol_len));
  if (id != kInvalidSymbol) sym_cache_.emplace(key, id);
  return id;
}

void FeedHandler::publish(const MarketEvent& ev) {
  // Apply backpressure rather than dropping: losing a quote silently would corrupt the book.
  if (queue_.try_push(ev)) return;
  stats_.backpressure.fetch_add(1, std::memory_order_relaxed);
  while (!queue_.try_push(ev)) {
    if (!running_.load(std::memory_order_relaxed) && thread_.joinable()) return;
    std::this_thread::yield();
  }
}

std::size_t FeedHandler::poll_once() {
  const ssize_t n = source_->read(buf_.data() + buf_len_, buf_.size() - buf_len_);
  if (n < 0) {
    finished_.store(true, std::memory_order_release);
  } else {
    buf_len_ += static_cast<std::size_t>(n);
    stats_.bytes.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
  }
  if (buf_len_ == 0) return 0;

  std::size_t published = 0;
  std::size_t off = 0;
  while (off < buf_len_) {
    const BatchResult r =
        cfg_.use_simd
            ? simd::parse_batch(buf_.data() + off, buf_len_ - off, parsed_.data(), parsed_.size())
            : scalar::parse_batch(buf_.data() + off, buf_len_ - off, parsed_.data(),
                                  parsed_.size());
    if (r.errors) stats_.parse_errors.fetch_add(r.errors, std::memory_order_relaxed);
    const Timestamp now = mono_ns();

    for (std::size_t i = 0; i < r.messages; ++i) {
      const ParsedMessage& m = parsed_[i];
      if (last_seq_ != 0 && m.seq <= last_seq_) {
        stats_.stale.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (last_seq_ != 0 && m.seq > last_seq_ + 1)
        stats_.gaps.fetch_add(m.seq - last_seq_ - 1, std::memory_order_relaxed);
      last_seq_ = m.seq;

      MarketEvent ev;
      ev.type = m.type;
      ev.exchange = cfg_.exchange;
      ev.symbol = resolve(m);
      if (ev.symbol == kInvalidSymbol) {
        stats_.parse_errors.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      ev.seq = m.seq;
      ev.exch_ts = m.exch_ts;
      ev.recv_ts = now;
      if (m.type == EventType::Quote) ev.quote = m.quote; else ev.trade = m.trade;
      publish(ev);
      ++published;
    }
    stats_.messages.fetch_add(r.messages, std::memory_order_relaxed);
    stats_.last_seq.store(last_seq_, std::memory_order_relaxed);
    if (r.consumed == 0) break;
    off += r.consumed;
  }

  // Keep the partial trailing message for the next read.
  if (off > 0) {
    std::memmove(buf_.data(), buf_.data() + off, buf_len_ - off);
    buf_len_ -= off;
  }
  if (finished()) buf_len_ = 0;  // source closed: discard any incomplete tail
  return published;
}

void FeedHandler::run() {
  pin_current_thread(cfg_.cpu_affinity);
  while (running_.load(std::memory_order_acquire) && !finished()) {
    if (poll_once() == 0 && !cfg_.busy_poll) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  finished_.store(true, std::memory_order_release);
}

}  // namespace qm
