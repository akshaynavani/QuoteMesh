#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace qm {

// Fixed-point price with 4 implied decimal places: 123.4567 -> 1234567.
using Price = std::int64_t;
inline constexpr int kPriceDecimals = 4;
inline constexpr Price kPriceScale = 10'000;
inline constexpr Price kNoBid = std::numeric_limits<Price>::min();
inline constexpr Price kNoAsk = std::numeric_limits<Price>::max();

using Quantity = std::uint32_t;
using SeqNum = std::uint64_t;
using Timestamp = std::uint64_t;  // nanoseconds since epoch
using SymbolId = std::uint32_t;
using ExchangeId = std::uint16_t;

inline constexpr SymbolId kInvalidSymbol = std::numeric_limits<SymbolId>::max();
inline constexpr std::size_t kMaxExchanges = 32;
inline constexpr std::size_t kMaxSymbolLen = 8;
inline constexpr std::size_t kCacheLine = 64;

constexpr double to_double(Price p) noexcept { return static_cast<double>(p) / kPriceScale; }

enum class EventType : std::uint8_t { Quote = 1, Trade = 2 };

struct Quote {
  Price bid_px{kNoBid};
  Price ask_px{kNoAsk};
  Quantity bid_sz{0};
  Quantity ask_sz{0};
};

struct Trade {
  Price px{0};
  Quantity sz{0};
};

// Normalised event that flows from feed handlers to the aggregator.
struct MarketEvent {
  EventType type{EventType::Quote};
  ExchangeId exchange{0};
  SymbolId symbol{kInvalidSymbol};
  SeqNum seq{0};
  Timestamp exch_ts{0};   // timestamp stamped by the exchange
  Timestamp recv_ts{0};   // timestamp when the feed handler parsed it
  union {
    Quote quote;
    Trade trade;
  };

  MarketEvent() noexcept : quote{} {}
};

// Raw parsed message before symbol interning (symbol kept as a short inline string).
struct ParsedMessage {
  EventType type{EventType::Quote};
  char symbol[kMaxSymbolLen]{};
  std::uint8_t symbol_len{0};
  SeqNum seq{0};
  Timestamp exch_ts{0};
  Quote quote{};
  Trade trade{};
};

}  // namespace qm
