#include "quotemesh/parser.hpp"

namespace qm {

const char* to_string(ParseError e) noexcept {
  switch (e) {
    case ParseError::None: return "none";
    case ParseError::BadType: return "bad_type";
    case ParseError::FieldCount: return "field_count";
    case ParseError::BadSeq: return "bad_seq";
    case ParseError::BadSymbol: return "bad_symbol";
    case ParseError::BadTimestamp: return "bad_timestamp";
    case ParseError::BadPrice: return "bad_price";
    case ParseError::BadSize: return "bad_size";
    case ParseError::Inconsistent: return "inconsistent";
  }
  return "unknown";
}

namespace scalar {

bool parse_uint(const char* p, std::size_t n, std::uint64_t& out) noexcept {
  if (n == 0 || n > 19) return false;
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const auto d = static_cast<unsigned>(p[i] - '0');
    if (d > 9) return false;
    v = v * 10 + d;
  }
  out = v;
  return true;
}

bool parse_price(const char* p, std::size_t n, Price& out) noexcept {
  if (n == 0) return false;
  std::size_t dot = n;
  for (std::size_t i = 0; i < n; ++i) {
    if (p[i] == '.') { dot = i; break; }
  }
  const std::size_t int_len = dot;
  const std::size_t frac_len = dot == n ? 0 : n - dot - 1;
  if (int_len == 0 || int_len > 14) return false;
  if (dot != n && (frac_len == 0 || frac_len > static_cast<std::size_t>(kPriceDecimals)))
    return false;

  std::uint64_t whole = 0;
  if (!parse_uint(p, int_len, whole)) return false;
  std::uint64_t frac = 0;
  if (frac_len > 0 && !parse_uint(p + dot + 1, frac_len, frac)) return false;
  for (std::size_t i = frac_len; i < static_cast<std::size_t>(kPriceDecimals); ++i) frac *= 10;
  out = static_cast<Price>(whole) * kPriceScale + static_cast<Price>(frac);
  return true;
}

namespace {
struct ScalarNum {
  static bool parse_uint(const char* p, std::size_t n, std::uint64_t& v) noexcept {
    return scalar::parse_uint(p, n, v);
  }
  static bool parse_price(const char* p, std::size_t n, Price& v) noexcept {
    return scalar::parse_price(p, n, v);
  }
};
}  // namespace

ParseError parse_line(std::string_view line, ParsedMessage& out) noexcept {
  if (line.size() > wire::kMaxMessageLen) return ParseError::FieldCount;
  FieldSpan fields[wire::kMaxFields];
  std::size_t nf = 0;
  std::uint16_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == wire::kDelim) {
      if (nf == wire::kMaxFields) return ParseError::FieldCount;
      fields[nf++] = {start, static_cast<std::uint16_t>(i)};
      start = static_cast<std::uint16_t>(i + 1);
    }
  }
  return detail::build_message<ScalarNum>(line.data(), fields, nf, out);
}

BatchResult parse_batch(const char* data, std::size_t len, ParsedMessage* out,
                        std::size_t max_out) noexcept {
  BatchResult r;
  std::size_t pos = 0;
  while (pos < len && r.messages < max_out) {
    const void* nl = std::memchr(data + pos, wire::kEol, len - pos);
    if (nl == nullptr) {
      // No terminator: if the tail is already longer than any legal message it is garbage.
      if (len - pos > wire::kMaxMessageLen) { r.errors++; pos = len; }
      break;
    }
    const auto end = static_cast<std::size_t>(static_cast<const char*>(nl) - data);
    if (parse_line({data + pos, end - pos}, out[r.messages]) == ParseError::None) {
      r.messages++;
    } else {
      r.errors++;
    }
    pos = end + 1;
  }
  r.consumed = pos;
  return r;
}

}  // namespace scalar
}  // namespace qm
