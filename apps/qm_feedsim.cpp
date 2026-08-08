// qm_feedsim: sends a synthetic exchange feed over UDP (unicast or multicast) so the
// aggregator's UDP ingestion path can be exercised without real exchange connectivity.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "quotemesh/config.hpp"
#include "quotemesh/synthetic_feed.hpp"
#include "quotemesh/udp_feed.hpp"

using namespace qm;

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

void usage(const char* argv0) {
  std::printf(
      "usage: %s --dest HOST:PORT [--rate MSGS_PER_SEC] [--symbols A,B,C] [--count N]\n"
      "          [--drop RATIO] [--seed N] [--name EXCH]\n",
      argv0);
}
}  // namespace

int main(int argc, char** argv) {
  std::string dest;
  SyntheticFeedConfig cfg;
  cfg.symbols = {"AAPL", "MSFT", "NVDA", "SPY", "QQQ"};
  cfg.messages_per_sec = 10'000;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has = i + 1 < argc;
    if (a == "--dest" && has) dest = argv[++i];
    else if (a == "--rate" && has) cfg.messages_per_sec = std::stod(argv[++i]);
    else if (a == "--symbols" && has) cfg.symbols = split(argv[++i], ',');
    else if (a == "--count" && has) cfg.max_messages = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--drop" && has) cfg.drop_ratio = std::stod(argv[++i]);
    else if (a == "--seed" && has) cfg.seed = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--name" && has) cfg.exchange_name = argv[++i];
    else { usage(argv[0]); return a == "-h" || a == "--help" ? 0 : 2; }
  }
  const auto colon = dest.rfind(':');
  if (dest.empty() || colon == std::string::npos) {
    usage(argv[0]);
    return 2;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  try {
    UdpSender sender(dest.substr(0, colon), static_cast<std::uint16_t>(std::stoi(dest.substr(colon + 1))));
    SyntheticFeed feed(cfg);
    char datagram[1400];  // stay under a typical Ethernet MTU
    std::uint64_t packets = 0, bytes = 0;
    auto last = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
      const ssize_t n = feed.read(datagram, sizeof datagram);
      if (n < 0) break;
      if (n == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }
      if (sender.send(datagram, static_cast<std::size_t>(n))) {
        ++packets;
        bytes += static_cast<std::uint64_t>(n);
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - last >= std::chrono::seconds(1)) {
        std::printf("sent %llu msgs, %llu packets, %.1f KB\n",
                    static_cast<unsigned long long>(feed.generated()),
                    static_cast<unsigned long long>(packets), static_cast<double>(bytes) / 1e3);
        std::fflush(stdout);
        last = now;
      }
    }
    std::printf("done: %llu messages in %llu packets\n",
                static_cast<unsigned long long>(feed.generated()),
                static_cast<unsigned long long>(packets));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
