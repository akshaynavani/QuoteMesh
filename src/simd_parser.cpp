#include <bit>

#include "quotemesh/parser.hpp"
#include "quotemesh/simd/delimiter_scan.hpp"
#include "quotemesh/simd/number_parse.hpp"

namespace qm::simd {
namespace {

struct SimdNum {
  static bool parse_uint(const char* p, std::size_t n, std::uint64_t& v) noexcept {
    return simd::parse_uint(p, n, v);
  }
  static bool parse_price(const char* p, std::size_t n, Price& v) noexcept {
    return simd::parse_price(p, n, v);
  }
};

}  // namespace

const char* backend_name() noexcept { return compiled_backend(); }

BatchResult parse_batch(const char* data, std::size_t len, ParsedMessage* out,
                        std::size_t max_out) noexcept {
  BatchResult r;
  if (max_out == 0 || len == 0) return r;

  std::size_t line_start = 0;
  FieldSpan fields[wire::kMaxFields];
  std::size_t nf = 0;
  std::uint16_t field_start = 0;
  bool bad = false;

  for (std::size_t block = 0; block < len; block += kBlockSize) {
    const std::size_t avail = len - block;
    const BlockMasks m = QM_LIKELY(avail >= kBlockSize) ? scan_block64(data + block)
                                                        : scan_tail(data + block, avail);
    std::uint64_t bits = m.delim | m.eol;
    while (bits != 0) {
      const int i = std::countr_zero(bits);
      bits &= bits - 1;
      const std::size_t pos = block + static_cast<std::size_t>(i);
      const std::size_t rel = pos - line_start;
      if (QM_UNLIKELY(rel > wire::kMaxMessageLen)) bad = true;

      if (!bad) {
        if (nf < wire::kMaxFields) {
          fields[nf++] = {field_start, static_cast<std::uint16_t>(rel)};
        } else {
          bad = true;  // too many fields
        }
      }
      field_start = static_cast<std::uint16_t>(rel + 1);

      if ((m.eol >> i) & 1u) {
        if (!bad && detail::build_message<SimdNum>(data + line_start, fields, nf,
                                                   out[r.messages]) == ParseError::None) {
          r.messages++;
        } else {
          r.errors++;
        }
        line_start = pos + 1;
        r.consumed = line_start;
        nf = 0;
        field_start = 0;
        bad = false;
        if (r.messages == max_out) return r;
      }
    }
  }

  // Unterminated tail longer than any valid message: drop it so the caller can make progress.
  if (len - line_start > wire::kMaxMessageLen) {
    r.errors++;
    r.consumed = len;
  }
  return r;
}

}  // namespace qm::simd
