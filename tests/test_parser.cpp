#include <gtest/gtest.h>

#include <string>

#include "quotemesh/parser.hpp"
#include "quotemesh/wire_format.hpp"

using namespace qm;

namespace {
std::string sym(const ParsedMessage& m) { return std::string(m.symbol, m.symbol_len); }
}  // namespace

TEST(WireFormat, FormatsPricesWithTrimmedDecimals) {
  char buf[32];
  EXPECT_EQ(std::string(buf, wire::format_price(buf, 1012500)), "101.25");
  EXPECT_EQ(std::string(buf, wire::format_price(buf, 420000)), "42");
  EXPECT_EQ(std::string(buf, wire::format_price(buf, 70001)), "7.0001");
  EXPECT_EQ(std::string(buf, wire::format_price(buf, 5)), "0.0005");
}

TEST(WireFormat, EncodesQuote) {
  char buf[wire::kMaxMessageLen];
  Quote q{1012500, 1013000, 300, 200};
  auto n = wire::encode_quote(buf, sizeof buf, 42, "AAPL", 1700000000000000000ull, q);
  EXPECT_EQ(std::string(buf, n), "Q|42|AAPL|1700000000000000000|101.25|300|101.3|200\n");
}

TEST(WireFormat, EncodesEmptySides) {
  char buf[wire::kMaxMessageLen];
  Quote q{kNoBid, 990000, 0, 10};
  auto n = wire::encode_quote(buf, sizeof buf, 1, "X", 5, q);
  EXPECT_EQ(std::string(buf, n), "Q|1|X|5||0|99|10\n");
}

TEST(ScalarParser, ParsesUint) {
  std::uint64_t v = 0;
  EXPECT_TRUE(scalar::parse_uint("12345", 5, v));
  EXPECT_EQ(v, 12345u);
  EXPECT_FALSE(scalar::parse_uint("12a45", 5, v));
  EXPECT_FALSE(scalar::parse_uint("", 0, v));
  EXPECT_FALSE(scalar::parse_uint("12345678901234567890", 20, v));
}

TEST(ScalarParser, ParsesPrices) {
  Price p = 0;
  EXPECT_TRUE(scalar::parse_price("101.25", 6, p));
  EXPECT_EQ(p, 1012500);
  EXPECT_TRUE(scalar::parse_price("42", 2, p));
  EXPECT_EQ(p, 420000);
  EXPECT_TRUE(scalar::parse_price("0.0001", 6, p));
  EXPECT_EQ(p, 1);
  EXPECT_FALSE(scalar::parse_price("1.23456", 7, p));
  EXPECT_FALSE(scalar::parse_price(".5", 2, p));
  EXPECT_FALSE(scalar::parse_price("5.", 2, p));
  EXPECT_FALSE(scalar::parse_price("1.2.3", 5, p));
}

TEST(ScalarParser, ParsesQuoteLine) {
  ParsedMessage m;
  ASSERT_EQ(scalar::parse_line("Q|7|MSFT|123|410.5|100|410.52|250", m), ParseError::None);
  EXPECT_EQ(m.type, EventType::Quote);
  EXPECT_EQ(m.seq, 7u);
  EXPECT_EQ(sym(m), "MSFT");
  EXPECT_EQ(m.exch_ts, 123u);
  EXPECT_EQ(m.quote.bid_px, 4105000);
  EXPECT_EQ(m.quote.bid_sz, 100u);
  EXPECT_EQ(m.quote.ask_px, 4105200);
  EXPECT_EQ(m.quote.ask_sz, 250u);
}

TEST(ScalarParser, ParsesTradeLine) {
  ParsedMessage m;
  ASSERT_EQ(scalar::parse_line("T|8|SPY|99|512.01|5", m), ParseError::None);
  EXPECT_EQ(m.type, EventType::Trade);
  EXPECT_EQ(m.trade.px, 5120100);
  EXPECT_EQ(m.trade.sz, 5u);
}

TEST(ScalarParser, RejectsMalformed) {
  ParsedMessage m;
  EXPECT_EQ(scalar::parse_line("X|1|A|1|1|1", m), ParseError::BadType);
  EXPECT_EQ(scalar::parse_line("Q|1|A|1|1|1", m), ParseError::FieldCount);
  EXPECT_EQ(scalar::parse_line("Q|x|A|1|1|1|1|1", m), ParseError::BadSeq);
  EXPECT_EQ(scalar::parse_line("Q|1|TOOLONGSYM|1|1|1|1|1", m), ParseError::BadSymbol);
  EXPECT_EQ(scalar::parse_line("Q|1|A|1|abc|1|1|1", m), ParseError::BadPrice);
  EXPECT_EQ(scalar::parse_line("Q|1|A|1||5|1|1", m), ParseError::Inconsistent);
}

TEST(ScalarParser, RoundTripsEncodedMessages) {
  char buf[wire::kMaxMessageLen];
  Quote q{1234567, 1234600, 17, 33};
  auto n = wire::encode_quote(buf, sizeof buf, 99, "QQQ", 1234, q);
  ParsedMessage m;
  ASSERT_EQ(scalar::parse_line({buf, n - 1}, m), ParseError::None);
  EXPECT_EQ(m.quote.bid_px, q.bid_px);
  EXPECT_EQ(m.quote.ask_px, q.ask_px);
  EXPECT_EQ(m.quote.bid_sz, q.bid_sz);
  EXPECT_EQ(m.quote.ask_sz, q.ask_sz);
}

TEST(ScalarParser, BatchStopsAtPartialLine) {
  const std::string data = "T|1|A|1|1|1\nbogus\nT|2|B|2|2|2\nT|3|C|3";
  ParsedMessage out[8];
  auto r = scalar::parse_batch(data.data(), data.size(), out, 8);
  EXPECT_EQ(r.messages, 2u);
  EXPECT_EQ(r.errors, 1u);
  EXPECT_EQ(r.consumed, data.find("T|3"));
}
