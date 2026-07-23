#pragma once

#include <cstddef>
#include <string_view>

#include "quotemesh/types.hpp"

// QuoteMesh normalised text wire format (one message per line, '|' delimited):
//
//   Quote: Q|<seq>|<symbol>|<exch_ts_ns>|<bid_px>|<bid_sz>|<ask_px>|<ask_sz>\n
//   Trade: T|<seq>|<symbol>|<exch_ts_ns>|<px>|<sz>\n
//
// Prices are decimal with up to 4 fractional digits ("101.25", "7.0001", "42").
// An empty price field means "no price on this side" (size must then be 0).
// Symbols are 1..8 printable characters.
namespace qm::wire {

inline constexpr char kDelim = '|';
inline constexpr char kEol = '\n';
inline constexpr std::size_t kQuoteFields = 8;
inline constexpr std::size_t kTradeFields = 6;
inline constexpr std::size_t kMaxFields = 8;
// Generous upper bound for a single encoded message.
inline constexpr std::size_t kMaxMessageLen = 128;

// Format a price into `out` (no terminator). Returns characters written.
std::size_t format_price(char* out, Price px);

// Encode messages into `out` (capacity >= kMaxMessageLen). Returns bytes written, 0 on error.
std::size_t encode_quote(char* out, std::size_t cap, SeqNum seq, std::string_view symbol,
                         Timestamp ts, const Quote& q);
std::size_t encode_trade(char* out, std::size_t cap, SeqNum seq, std::string_view symbol,
                         Timestamp ts, const Trade& t);

}  // namespace qm::wire
