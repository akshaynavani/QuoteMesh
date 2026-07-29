// Parser micro-benchmark: scalar reference vs SIMD batch parser on a synthetic feed.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "quotemesh/parser.hpp"
#include "quotemesh/wire_format.hpp"

using namespace qm;
using Clock = std::chrono::steady_clock;

namespace {

std::string make_feed(std::size_t count) {
  static const char* syms[] = {"AAPL", "MSFT", "SPY", "QQQ", "NVDA", "TSLA", "AMZN", "META"};
  std::mt19937_64 rng(42);
  std::string out;
  out.reserve(count * 64);
  char buf[wire::kMaxMessageLen];
  Price mid = 1'000'000;
  for (std::size_t i = 0; i < count; ++i) {
    mid += static_cast<Price>(rng() % 21) - 10;
    const Timestamp ts = 1'700'000'000'000'000'000ull + i * 1000;
    Quote q{mid - 50, mid + 50, static_cast<Quantity>(rng() % 900 + 100),
            static_cast<Quantity>(rng() % 900 + 100)};
    out.append(buf, wire::encode_quote(buf, sizeof buf, i + 1, syms[rng() % 8], ts, q));
  }
  return out;
}

template <typename Fn>
void run(const char* name, const std::string& feed, int reps, Fn&& fn) {
  std::vector<ParsedMessage> out(1024);
  std::size_t total = 0;
  const auto t0 = Clock::now();
  for (int r = 0; r < reps; ++r) {
    std::size_t off = 0;
    while (off < feed.size()) {
      const auto res = fn(feed.data() + off, feed.size() - off, out.data(), out.size());
      if (res.consumed == 0) break;
      off += res.consumed;
      total += res.messages;
    }
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  const double bytes = static_cast<double>(feed.size()) * reps;
  std::printf("%-8s %10.2f Mmsg/s %8.2f GB/s %7.1f ns/msg  (%zu msgs)\n", name,
              static_cast<double>(total) / secs / 1e6, bytes / secs / 1e9,
              secs * 1e9 / static_cast<double>(total), total);
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
  const int reps = argc > 2 ? std::atoi(argv[2]) : 10;
  const std::string feed = make_feed(count);
  std::printf("feed: %zu messages, %.1f MB, SIMD backend: %s\n", count,
              static_cast<double>(feed.size()) / 1e6, simd::backend_name());
  run("scalar", feed, reps, scalar::parse_batch);
  run("simd", feed, reps, simd::parse_batch);
  return 0;
}
