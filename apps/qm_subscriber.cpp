// qm_subscriber: example downstream consumer. Attaches read-only to the QuoteMesh shared
// memory segment and either tails the NBBO/trade stream or prints a snapshot table.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "quotemesh/clock.hpp"
#include "quotemesh/shm/subscriber.hpp"

using namespace qm;
using namespace qm::shm;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

std::string px(Price p) {
  if (p == kNoBid || p == kNoAsk) return "-";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.4f", to_double(p));
  return buf;
}

std::string venues(const ShmSubscriber& sub, std::uint32_t mask) {
  std::string out;
  for (std::uint8_t i = 0; i < kMaxExchanges; ++i) {
    if (mask & (1u << i)) {
      if (!out.empty()) out += ',';
      out += sub.exchange_name(i);
    }
  }
  return out;
}

void print_snapshot(const ShmSubscriber& sub) {
  std::printf("%-6s %12s %8s %12s %8s %12s %12s  %s\n", "SYM", "BID", "BIDSZ", "ASK", "ASKSZ",
              "LAST", "VOLUME", "BEST VENUES (bid | ask)");
  const std::uint32_t n = sub.symbol_count();
  for (SymbolId s = 0; s < n; ++s) {
    SnapshotData d{};
    if (!sub.snapshot(s, d)) continue;
    std::printf("%-6.8s %12s %8u %12s %8u %12s %12llu  %s | %s%s\n", d.symbol, px(d.bid_px).c_str(),
                d.bid_sz, px(d.ask_px).c_str(), d.ask_sz, px(d.last_px).c_str(),
                static_cast<unsigned long long>(d.volume), venues(sub, d.bid_venues).c_str(),
                venues(sub, d.ask_venues).c_str(), (d.flags & kFlagCrossed) ? "  CROSSED" : "");
  }
}

void usage(const char* argv0) {
  std::printf(
      "usage: %s [--shm NAME] [--snapshot] [--symbol SYM] [--stats] [--from-oldest]\n"
      "  --snapshot     print the consolidated table once and exit\n"
      "  --symbol SYM   only print updates for this symbol\n"
      "  --stats        print throughput/latency once per second instead of every message\n",
      argv0);
}

}  // namespace

int main(int argc, char** argv) {
  std::string name = std::getenv("QUOTEMESH_SHM_NAME") ? std::getenv("QUOTEMESH_SHM_NAME")
                                                       : "/quotemesh";
  bool snapshot = false, stats_only = false, from_oldest = false;
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--shm" && i + 1 < argc) name = argv[++i];
    else if (a == "--snapshot") snapshot = true;
    else if (a == "--symbol" && i + 1 < argc) filter = argv[++i];
    else if (a == "--stats") stats_only = true;
    else if (a == "--from-oldest") from_oldest = true;
    else { usage(argv[0]); return a == "-h" || a == "--help" ? 0 : 2; }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  try {
    ShmSubscriber sub = ShmSubscriber::open(name, !from_oldest);
    std::fprintf(stderr, "attached to %s (writer pid %lld, ring %u slots)\n", name.c_str(),
                 static_cast<long long>(sub.header().writer_pid), sub.header().ring_capacity);
    if (snapshot) {
      print_snapshot(sub);
      return 0;
    }

    std::optional<SymbolId> only;
    Payload p{};
    std::uint64_t count = 0, window = 0, lat_sum = 0;
    auto last_report = std::chrono::steady_clock::now();
    std::uint32_t idle = 0;
    while (!g_stop.load()) {
      const ReadStatus st = sub.poll(p);
      if (st == ReadStatus::Empty) {
        if (++idle > 1000) std::this_thread::sleep_for(std::chrono::microseconds(50));
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report > std::chrono::seconds(2) && !sub.writer_alive(wall_ns())) {
          std::fprintf(stderr, "publisher heartbeat lost\n");
          last_report = now;
        }
        continue;
      }
      idle = 0;
      if (st == ReadStatus::Overrun) {
        std::fprintf(stderr, "overrun: %llu messages lost so far\n",
                     static_cast<unsigned long long>(sub.lost()));
        continue;
      }
      ++count;
      ++window;
      const Timestamp now_wall = wall_ns();
      lat_sum += now_wall > p.publish_ts ? now_wall - p.publish_ts : 0;

      if (!filter.empty() && !only) only = sub.find_symbol(filter);
      if (stats_only) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
          std::printf("msgs/s=%llu total=%llu lost=%llu lag=%llu shm->sub mean=%.0fns\n",
                      static_cast<unsigned long long>(window),
                      static_cast<unsigned long long>(count),
                      static_cast<unsigned long long>(sub.lost()),
                      static_cast<unsigned long long>(sub.lag()),
                      window ? static_cast<double>(lat_sum) / static_cast<double>(window) : 0.0);
          std::fflush(stdout);
          window = 0;
          lat_sum = 0;
          last_report = now;
        }
        continue;
      }
      if (!filter.empty() && (!only || p.symbol != *only)) continue;
      const std::string sym = sub.symbol_name(p.symbol);
      if (p.type == MsgType::Nbbo) {
        std::printf("NBBO  %-6s %10s x %-6u  %10s x %-6u  bid@%s ask@%s%s\n", sym.c_str(),
                    px(p.bid_px).c_str(), p.bid_sz, px(p.ask_px).c_str(), p.ask_sz,
                    venues(sub, p.bid_venues).c_str(), venues(sub, p.ask_venues).c_str(),
                    (p.flags & kFlagCrossed) ? " CROSSED" : (p.flags & kFlagLocked) ? " LOCKED" : "");
      } else if (p.type == MsgType::Trade) {
        std::printf("TRADE %-6s %10s x %-6u  on %s\n", sym.c_str(), px(p.bid_px).c_str(), p.bid_sz,
                    sub.exchange_name(p.exchange).c_str());
      } else {
        std::printf("VENUE %-6s %-6s %10s / %-10s\n", sym.c_str(),
                    sub.exchange_name(p.exchange).c_str(), px(p.bid_px).c_str(),
                    px(p.ask_px).c_str());
      }
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s (is the quotemesh aggregator running?)\n", e.what());
    return 1;
  }
  return 0;
}
