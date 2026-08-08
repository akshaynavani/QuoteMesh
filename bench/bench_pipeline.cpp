// End-to-end throughput: N synthetic feeds (unthrottled) -> SIMD parse -> SPSC -> aggregator
// -> shared-memory publisher (anonymous mapping), plus one concurrent shm reader.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "quotemesh/aggregator.hpp"
#include "quotemesh/config.hpp"
#include "quotemesh/shm/publisher.hpp"
#include "quotemesh/shm/subscriber.hpp"
#include "quotemesh/synthetic_feed.hpp"

using namespace qm;

int main(int argc, char** argv) {
  const std::size_t n_feeds = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 16;
  const std::uint64_t per_feed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1'000'000;
  const bool use_simd = !(argc > 3 && std::string(argv[3]) == "scalar");
  const AppConfig defaults = default_config();

  SymbolTable symbols;
  for (const auto& s : defaults.symbols) symbols.intern(s);

  std::vector<std::unique_ptr<FeedHandler>> feeds;
  std::vector<FeedHandler*> raw;
  std::vector<std::string> names;
  for (std::size_t i = 0; i < n_feeds && i < kMaxExchanges; ++i) {
    SyntheticFeedConfig sc;
    sc.exchange_name = "EX" + std::to_string(i);
    sc.symbols = defaults.symbols;
    sc.messages_per_sec = 0;  // as fast as possible
    sc.max_messages = per_feed;
    sc.seed = i + 1;
    FeedHandlerConfig fc;
    fc.exchange = static_cast<ExchangeId>(i);
    fc.name = sc.exchange_name;
    fc.use_simd = use_simd;
    feeds.push_back(std::make_unique<FeedHandler>(fc, std::make_unique<SyntheticFeed>(sc), symbols));
    raw.push_back(feeds.back().get());
    names.push_back(sc.exchange_name);
  }

  shm::Geometry geo{1u << 18, 1024};
  shm::ShmPublisher pub(geo, shm::ShmRegion::anonymous(geo.total_size()), symbols, names);
  shm::ShmSubscriber sub(pub.base());

  std::atomic<bool> reading{true};
  std::uint64_t read_count = 0;
  std::thread reader([&] {
    shm::Payload p{};
    while (reading.load(std::memory_order_relaxed)) {
      if (sub.poll(p) == shm::ReadStatus::Ok) ++read_count;
    }
    while (sub.poll(p) == shm::ReadStatus::Ok) ++read_count;
  });

  Aggregator agg({}, raw, pub);
  const auto t0 = std::chrono::steady_clock::now();
  agg.start();
  for (auto& f : feeds) f->start();
  for (auto& f : feeds) {
    while (!f->finished()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  agg.stop();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  reading.store(false);
  reader.join();

  const auto& s = agg.stats();
  const double events = static_cast<double>(s.events.load());
  std::printf("feeds=%zu parser=%s backend=%s\n", feeds.size(), use_simd ? "simd" : "scalar",
              simd::backend_name());
  std::printf("events=%.0f in %.3fs -> %.2f M events/s\n", events, secs, events / secs / 1e6);
  std::printf("nbbo changes=%llu trades=%llu shm published=%llu read=%llu lost=%llu\n",
              static_cast<unsigned long long>(s.nbbo_changes.load()),
              static_cast<unsigned long long>(s.trades.load()),
              static_cast<unsigned long long>(pub.published()),
              static_cast<unsigned long long>(read_count),
              static_cast<unsigned long long>(sub.lost()));
  std::printf("feed->book latency p50<=%lluns p99<=%lluns mean=%.0fns\n",
              static_cast<unsigned long long>(s.feed_to_book_ns.percentile(0.5)),
              static_cast<unsigned long long>(s.feed_to_book_ns.percentile(0.99)),
              s.feed_to_book_ns.mean());
  return 0;
}
