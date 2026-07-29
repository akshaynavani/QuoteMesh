#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "quotemesh/parser.hpp"
#include "quotemesh/simd/delimiter_scan.hpp"
#include "quotemesh/simd/number_parse.hpp"
#include "quotemesh/wire_format.hpp"

using namespace qm;

TEST(SimdScan, MasksMatchByteScan) {
  std::mt19937 rng(7);
  const char alphabet[] = "0123456789|\nABC.";
  for (int iter = 0; iter < 200; ++iter) {
    char block[64];
    for (char& c : block) c = alphabet[rng() % (sizeof(alphabet) - 1)];
    const auto m = simd::scan_block64(block);
    for (int i = 0; i < 64; ++i) {
      EXPECT_EQ((m.delim >> i) & 1u, block[i] == '|' ? 1u : 0u);
      EXPECT_EQ((m.eol >> i) & 1u, block[i] == '\n' ? 1u : 0u);
    }
  }
}

TEST(SimdNumbers, ParseUintAllLengths) {
  std::mt19937_64 rng(11);
  for (std::size_t n = 1; n <= 19; ++n) {
    for (int k = 0; k < 50; ++k) {
      std::string s;
      for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>('0' + rng() % 10));
      std::uint64_t a = 0, b = 0;
      ASSERT_TRUE(scalar::parse_uint(s.data(), n, a));
      ASSERT_TRUE(simd::parse_uint(s.data(), n, b)) << s;
      EXPECT_EQ(a, b) << s;
    }
  }
}

TEST(SimdNumbers, RejectsNonDigits) {
  std::uint64_t v = 0;
  for (const char* s : {"12a", "1234567/", "12345678:9", "123456789012345x7", " 1", "1.0"}) {
    EXPECT_FALSE(simd::parse_uint(s, std::strlen(s), v)) << s;
  }
}

TEST(SimdNumbers, PricesMatchScalar) {
  for (const char* s : {"0", "1", "42", "101.25", "7.0001", "0.5", "99999999999999.9999",
                        "1.23456", ".5", "5.", "1.2.3", "12a.5", "3.x"}) {
    Price a = -1, b = -1;
    const bool ra = scalar::parse_price(s, std::strlen(s), a);
    const bool rb = simd::parse_price(s, std::strlen(s), b);
    EXPECT_EQ(ra, rb) << s;
    if (ra && rb) EXPECT_EQ(a, b) << s;
  }
}

namespace {

std::string random_feed(std::mt19937_64& rng, std::size_t count, bool inject_garbage) {
  static const char* syms[] = {"AAPL", "MSFT", "SPY", "QQQ", "NVDA", "TSLA", "A", "BRKB"};
  std::string out;
  char buf[wire::kMaxMessageLen];
  for (std::size_t i = 0; i < count; ++i) {
    const char* sym = syms[rng() % 8];
    const Timestamp ts = 1'700'000'000'000'000'000ull + rng() % 1'000'000'000ull;
    std::size_t n;
    if (rng() % 4 == 0) {
      Trade t{static_cast<Price>(rng() % 50'000'000), static_cast<Quantity>(rng() % 10'000)};
      n = wire::encode_trade(buf, sizeof buf, i + 1, sym, ts, t);
    } else {
      Price bid = static_cast<Price>(rng() % 50'000'000);
      Quote q{bid, bid + static_cast<Price>(rng() % 500 + 1),
              static_cast<Quantity>(rng() % 5000 + 1), static_cast<Quantity>(rng() % 5000 + 1)};
      n = wire::encode_quote(buf, sizeof buf, i + 1, sym, ts, q);
    }
    out.append(buf, n);
    if (inject_garbage && rng() % 10 == 0) {
      const auto pos = out.size() - 1 - rng() % (n - 1);
      out[pos] = "x|.\n9"[rng() % 5];
    }
  }
  return out;
}

void expect_same(const ParsedMessage& a, const ParsedMessage& b) {
  EXPECT_EQ(a.type, b.type);
  EXPECT_EQ(a.seq, b.seq);
  EXPECT_EQ(std::string(a.symbol, a.symbol_len), std::string(b.symbol, b.symbol_len));
  EXPECT_EQ(a.exch_ts, b.exch_ts);
  if (a.type == EventType::Quote) {
    EXPECT_EQ(a.quote.bid_px, b.quote.bid_px);
    EXPECT_EQ(a.quote.ask_px, b.quote.ask_px);
    EXPECT_EQ(a.quote.bid_sz, b.quote.bid_sz);
    EXPECT_EQ(a.quote.ask_sz, b.quote.ask_sz);
  } else {
    EXPECT_EQ(a.trade.px, b.trade.px);
    EXPECT_EQ(a.trade.sz, b.trade.sz);
  }
}

void differential(bool garbage, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  const std::string feed = random_feed(rng, 2000, garbage);
  std::vector<ParsedMessage> a(4096), b(4096);
  const auto ra = scalar::parse_batch(feed.data(), feed.size(), a.data(), a.size());
  const auto rb = simd::parse_batch(feed.data(), feed.size(), b.data(), b.size());
  ASSERT_EQ(ra.messages, rb.messages);
  EXPECT_EQ(ra.errors, rb.errors);
  EXPECT_EQ(ra.consumed, rb.consumed);
  for (std::size_t i = 0; i < ra.messages; ++i) expect_same(a[i], b[i]);
}

}  // namespace

TEST(SimdParser, MatchesScalarOnCleanFeed) {
  for (std::uint64_t seed = 1; seed <= 5; ++seed) differential(false, seed);
}

TEST(SimdParser, MatchesScalarOnCorruptedFeed) {
  for (std::uint64_t seed = 100; seed <= 110; ++seed) differential(true, seed);
}

TEST(SimdParser, RespectsOutputCapacityAndPartialLines) {
  std::mt19937_64 rng(3);
  std::string feed = random_feed(rng, 100, false);
  feed += "Q|101|AAPL|17";  // incomplete trailing message
  std::vector<ParsedMessage> out(40);
  auto r = simd::parse_batch(feed.data(), feed.size(), out.data(), out.size());
  EXPECT_EQ(r.messages, 40u);
  std::size_t total = r.messages;
  std::size_t off = r.consumed;
  while (true) {
    r = simd::parse_batch(feed.data() + off, feed.size() - off, out.data(), out.size());
    total += r.messages;
    off += r.consumed;
    if (r.messages == 0) break;
  }
  EXPECT_EQ(total, 100u);
  EXPECT_EQ(feed.substr(off), "Q|101|AAPL|17");
}

TEST(SimdParser, ReportsBackend) {
  EXPECT_NE(std::string(simd::backend_name()), "");
}
