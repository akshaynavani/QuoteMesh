// quotemesh: the aggregator daemon. Starts one feed-handler thread per configured exchange,
// one aggregator thread, and publishes consolidated NBBO/trades into shared memory.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "quotemesh/aggregator.hpp"
#include "quotemesh/clock.hpp"
#include "quotemesh/config.hpp"
#include "quotemesh/feed_handler.hpp"
#include "quotemesh/shm/publisher.hpp"
#include "quotemesh/synthetic_feed.hpp"
#include "quotemesh/udp_feed.hpp"

using namespace qm;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

void usage(const char* argv0) {
  std::printf(
      "usage: %s [--config FILE] [--set key=value]... [--duration SECONDS] [--scalar]\n"
      "  Runs the QuoteMesh aggregator. Without --config, 16 synthetic exchanges are used.\n",
      argv0);
}

std::unique_ptr<FeedSource> make_source(const AppConfig& cfg, const ExchangeConfig& ex,
                                        std::size_t idx) {
  if (ex.kind == ExchangeConfig::Kind::Udp) {
    UdpFeedConfig u;
    u.port = ex.udp_port;
    if (ex.multicast) u.multicast_group = ex.udp_host; else u.bind_addr = ex.udp_host;
    return std::make_unique<UdpFeed>(u);
  }
  SyntheticFeedConfig s;
  s.exchange_name = ex.name;
  s.symbols = cfg.symbols;
  s.messages_per_sec = cfg.synthetic_rate;
  s.trade_ratio = cfg.trade_ratio;
  s.drop_ratio = cfg.drop_ratio;
  s.seed = 0x9E3779B97F4A7C15ull * (idx + 1);
  return std::make_unique<SyntheticFeed>(s);
}

void print_stats(const std::vector<std::unique_ptr<FeedHandler>>& feeds, const Aggregator& agg,
                 const shm::ShmPublisher& pub, double elapsed, std::uint64_t& last_events) {
  std::uint64_t msgs = 0, errs = 0, gaps = 0, bp = 0, bytes = 0;
  for (const auto& f : feeds) {
    msgs += f->stats().messages.load();
    errs += f->stats().parse_errors.load();
    gaps += f->stats().gaps.load();
    bp += f->stats().backpressure.load();
    bytes += f->stats().bytes.load();
  }
  const auto& s = agg.stats();
  const std::uint64_t events = s.events.load();
  std::printf(
      "[%7.1fs] in=%llu msgs (%.1f MB) evt/s=%llu nbbo=%llu trades=%llu shm=%llu | "
      "gaps=%llu errs=%llu bp=%llu | feed->book p50=%lluns p99=%lluns mean=%.0fns\n",
      elapsed, static_cast<unsigned long long>(msgs), static_cast<double>(bytes) / 1e6,
      static_cast<unsigned long long>(events - last_events),
      static_cast<unsigned long long>(s.nbbo_changes.load()),
      static_cast<unsigned long long>(s.trades.load()),
      static_cast<unsigned long long>(pub.published()), static_cast<unsigned long long>(gaps),
      static_cast<unsigned long long>(errs), static_cast<unsigned long long>(bp),
      static_cast<unsigned long long>(s.feed_to_book_ns.percentile(0.5)),
      static_cast<unsigned long long>(s.feed_to_book_ns.percentile(0.99)),
      s.feed_to_book_ns.mean());
  std::fflush(stdout);
  last_events = events;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::vector<std::string> overrides;
  double duration = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) config_path = argv[++i];
    else if (a == "--set" && i + 1 < argc) overrides.emplace_back(argv[++i]);
    else if (a == "--duration" && i + 1 < argc) duration = std::stod(argv[++i]);
    else if (a == "--scalar") overrides.emplace_back("feed.simd=false");
    else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
    else { usage(argv[0]); return 2; }
  }

  AppConfig cfg;
  try {
    cfg = config_path.empty() ? default_config() : load_config(config_path);
    apply_env_overrides(cfg);
    for (const auto& kv : overrides) {
      const auto eq = kv.find('=');
      if (eq == std::string::npos) throw std::invalid_argument("--set expects key=value");
      cfg.set(trim(kv.substr(0, eq)), trim(kv.substr(eq + 1)));
    }
    cfg.validate();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "config error: %s\n", e.what());
    return 2;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::printf("QuoteMesh starting: %s, SIMD backend=%s\n", cfg.summary().c_str(),
              simd::backend_name());

  SymbolTable symbols(cfg.max_symbols);
  for (const auto& s : cfg.symbols) symbols.intern(s);  // stable ids for known symbols

  std::vector<std::string> names;
  for (const auto& e : cfg.exchanges) names.push_back(e.name);

  std::unique_ptr<shm::ShmPublisher> publisher;
  std::vector<std::unique_ptr<FeedHandler>> feeds;
  try {
    shm::PublisherConfig pc;
    pc.name = cfg.shm_name;
    pc.ring_capacity = cfg.ring_capacity;
    pc.max_symbols = cfg.max_symbols;
    pc.lock_pages = cfg.lock_pages;
    publisher = shm::ShmPublisher::create(pc, symbols, names);

    for (std::size_t i = 0; i < cfg.exchanges.size(); ++i) {
      FeedHandlerConfig fc;
      fc.exchange = static_cast<ExchangeId>(i);
      fc.name = cfg.exchanges[i].name;
      fc.queue_capacity = cfg.queue_capacity;
      fc.use_simd = cfg.use_simd;
      fc.busy_poll = cfg.feed_busy_poll;
      fc.cpu_affinity = cfg.first_feed_cpu >= 0 ? cfg.first_feed_cpu + static_cast<int>(i) : -1;
      feeds.push_back(std::make_unique<FeedHandler>(fc, make_source(cfg, cfg.exchanges[i], i),
                                                    symbols));
      std::printf("  feed %2zu: %s\n", i, feeds.back()->describe().c_str());
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "startup error: %s\n", e.what());
    return 1;
  }

  std::vector<FeedHandler*> raw;
  for (auto& f : feeds) raw.push_back(f.get());
  AggregatorConfig ac;
  ac.cpu_affinity = cfg.aggregator_cpu;
  ac.busy_poll = cfg.aggregator_busy_poll;
  ac.forward_venue_quotes = cfg.forward_venue_quotes;
  Aggregator agg(ac, raw, *publisher);

  agg.start();
  for (auto& f : feeds) f->start();
  std::printf("publishing to shared memory %s (%zu bytes)\n", publisher->region().name().c_str(),
              publisher->region().size());

  const auto t0 = std::chrono::steady_clock::now();
  auto next_report = t0 + std::chrono::milliseconds(cfg.stats_interval_ms);
  std::uint64_t last_events = 0;
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - t0).count();
    if (now >= next_report) {
      print_stats(feeds, agg, *publisher, elapsed, last_events);
      next_report += std::chrono::milliseconds(cfg.stats_interval_ms);
    }
    if (duration > 0 && elapsed >= duration) break;
  }

  std::printf("shutting down...\n");
  for (auto& f : feeds) f->stop();
  agg.stop();
  print_stats(feeds, agg, *publisher, std::chrono::duration<double>(
                                          std::chrono::steady_clock::now() - t0).count(),
              last_events);
  return 0;
}
