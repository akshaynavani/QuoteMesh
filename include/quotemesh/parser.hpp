#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "quotemesh/types.hpp"
#include "quotemesh/wire_format.hpp"

namespace qm {

enum class ParseError : std::uint8_t {
  None = 0,
  BadType,
  FieldCount,
  BadSeq,
  BadSymbol,
  BadTimestamp,
  BadPrice,
  BadSize,
  Inconsistent,  // e.g. a size without a price
};

const char* to_string(ParseError e) noexcept;

struct BatchResult {
  std::size_t consumed{0};  // bytes consumed (always ends on a message boundary)
  std::size_t messages{0};  // messages written to the output array
  std::size_t errors{0};    // malformed lines skipped
};

struct FieldSpan {
  std::uint16_t begin;
  std::uint16_t end;  // exclusive
  constexpr std::size_t size() const noexcept { return static_cast<std::size_t>(end - begin); }
};

namespace scalar {
// Parse an unsigned decimal of 1..19 digits.
bool parse_uint(const char* p, std::size_t n, std::uint64_t& out) noexcept;
// Parse a non-negative decimal price with up to kPriceDecimals fractional digits (n >= 1).
bool parse_price(const char* p, std::size_t n, Price& out) noexcept;
// Parse a single line (without the trailing '\n').
ParseError parse_line(std::string_view line, ParsedMessage& out) noexcept;
// Parse as many complete lines from `data` as fit in `out`.
BatchResult parse_batch(const char* data, std::size_t len, ParsedMessage* out,
                        std::size_t max_out) noexcept;
}  // namespace scalar

namespace simd {
// Same contract as scalar::parse_batch but uses vectorised delimiter scanning and digit parsing.
BatchResult parse_batch(const char* data, std::size_t len, ParsedMessage* out,
                        std::size_t max_out) noexcept;
// Name of the SIMD backend compiled in ("avx2", "sse2", "neon" or "scalar").
const char* backend_name() noexcept;
}  // namespace simd

namespace detail {

// Turns located fields into a ParsedMessage. `Num` supplies parse_uint/parse_price so the
// scalar and SIMD parsers share validation logic but use their own number decoders.
template <typename Num>
ParseError build_message(const char* line, const FieldSpan* f, std::size_t nfields,
                         ParsedMessage& out) noexcept {
  if (nfields < 1 || f[0].size() != 1) return ParseError::BadType;
  const char type = line[f[0].begin];
  if (type == 'Q') {
    if (nfields != wire::kQuoteFields) return ParseError::FieldCount;
    out.type = EventType::Quote;
  } else if (type == 'T') {
    if (nfields != wire::kTradeFields) return ParseError::FieldCount;
    out.type = EventType::Trade;
  } else {
    return ParseError::BadType;
  }

  std::uint64_t v = 0;
  if (!Num::parse_uint(line + f[1].begin, f[1].size(), v)) return ParseError::BadSeq;
  out.seq = v;

  const std::size_t sym_len = f[2].size();
  if (sym_len == 0 || sym_len > kMaxSymbolLen) return ParseError::BadSymbol;
  std::memset(out.symbol, 0, kMaxSymbolLen);
  std::memcpy(out.symbol, line + f[2].begin, sym_len);
  out.symbol_len = static_cast<std::uint8_t>(sym_len);

  if (!Num::parse_uint(line + f[3].begin, f[3].size(), v)) return ParseError::BadTimestamp;
  out.exch_ts = v;

  auto side = [&](const FieldSpan& pf, const FieldSpan& sf, Price empty, Price& px,
                  Quantity& sz) -> ParseError {
    std::uint64_t s = 0;
    if (!Num::parse_uint(line + sf.begin, sf.size(), s) || s > 0xFFFFFFFFull)
      return ParseError::BadSize;
    if (pf.size() == 0) {
      if (s != 0) return ParseError::Inconsistent;
      px = empty;
    } else if (!Num::parse_price(line + pf.begin, pf.size(), px)) {
      return ParseError::BadPrice;
    }
    sz = static_cast<Quantity>(s);
    return ParseError::None;
  };

  if (out.type == EventType::Quote) {
    if (auto e = side(f[4], f[5], kNoBid, out.quote.bid_px, out.quote.bid_sz); e != ParseError::None)
      return e;
    if (auto e = side(f[6], f[7], kNoAsk, out.quote.ask_px, out.quote.ask_sz); e != ParseError::None)
      return e;
  } else {
    if (f[4].size() == 0) return ParseError::BadPrice;
    if (auto e = side(f[4], f[5], 0, out.trade.px, out.trade.sz); e != ParseError::None) return e;
  }
  return ParseError::None;
}

}  // namespace detail
}  // namespace qm
