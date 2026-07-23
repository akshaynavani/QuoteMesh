#include "quotemesh/wire_format.hpp"

#include <charconv>
#include <cstring>

namespace qm::wire {
namespace {

struct Writer {
  char* p;
  char* end;
  bool ok{true};

  void put(char c) {
    if (p < end) *p++ = c; else ok = false;
  }
  void put(std::string_view s) {
    if (static_cast<std::size_t>(end - p) < s.size()) { ok = false; return; }
    std::memcpy(p, s.data(), s.size());
    p += s.size();
  }
  template <typename T>
  void num(T v) {
    auto [ptr, ec] = std::to_chars(p, end, v);
    if (ec != std::errc{}) { ok = false; return; }
    p = ptr;
  }
  void price(Price px) {
    if (px == kNoBid || px == kNoAsk) return;  // empty field
    if (end - p < 24) { ok = false; return; }
    p += format_price(p, px);
  }
};

}  // namespace

std::size_t format_price(char* out, Price px) {
  char* p = out;
  if (px < 0) { *p++ = '-'; px = -px; }
  const Price whole = px / kPriceScale;
  Price frac = px % kPriceScale;
  p = std::to_chars(p, p + 20, whole).ptr;
  if (frac != 0) {
    *p++ = '.';
    char digits[kPriceDecimals];
    for (int i = kPriceDecimals - 1; i >= 0; --i) {
      digits[i] = static_cast<char>('0' + frac % 10);
      frac /= 10;
    }
    int n = kPriceDecimals;
    while (n > 1 && digits[n - 1] == '0') --n;  // trim trailing zeros
    std::memcpy(p, digits, static_cast<std::size_t>(n));
    p += n;
  }
  return static_cast<std::size_t>(p - out);
}

std::size_t encode_quote(char* out, std::size_t cap, SeqNum seq, std::string_view symbol,
                         Timestamp ts, const Quote& q) {
  if (symbol.empty() || symbol.size() > kMaxSymbolLen) return 0;
  Writer w{out, out + cap};
  w.put('Q'); w.put(kDelim);
  w.num(seq); w.put(kDelim);
  w.put(symbol); w.put(kDelim);
  w.num(ts); w.put(kDelim);
  w.price(q.bid_px); w.put(kDelim);
  w.num(q.bid_sz); w.put(kDelim);
  w.price(q.ask_px); w.put(kDelim);
  w.num(q.ask_sz); w.put(kEol);
  return w.ok ? static_cast<std::size_t>(w.p - out) : 0;
}

std::size_t encode_trade(char* out, std::size_t cap, SeqNum seq, std::string_view symbol,
                         Timestamp ts, const Trade& t) {
  if (symbol.empty() || symbol.size() > kMaxSymbolLen) return 0;
  Writer w{out, out + cap};
  w.put('T'); w.put(kDelim);
  w.num(seq); w.put(kDelim);
  w.put(symbol); w.put(kDelim);
  w.num(ts); w.put(kDelim);
  w.price(t.px); w.put(kDelim);
  w.num(t.sz); w.put(kEol);
  return w.ok ? static_cast<std::size_t>(w.p - out) : 0;
}

}  // namespace qm::wire
