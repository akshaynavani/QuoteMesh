#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "quotemesh/simd/arch.hpp"
#include "quotemesh/types.hpp"

// Branch-light decimal parsing. Short fields use SWAR (SIMD-within-a-register) on a single
// 64-bit word; long fields such as nanosecond timestamps use a 16-lane vector multiply-add
// reduction (SSE4.1 or NEON) with a SWAR fallback.
namespace qm::simd {

static_assert(std::endian::native == std::endian::little,
              "SWAR digit parsing assumes a little-endian target");

inline constexpr std::uint64_t kAsciiZeros8 = 0x3030303030303030ull;

// True if all 8 bytes of `v` are ASCII digits.
QM_ALWAYS_INLINE bool all_digits8(std::uint64_t v) noexcept {
  return (((v & 0xF0F0F0F0F0F0F0F0ull) |
           (((v + 0x0606060606060606ull) & 0xF0F0F0F0F0F0F0F0ull) >> 4)) ==
          0x3333333333333333ull);
}

// Convert 8 ASCII digits (first byte most significant) to an integer.
QM_ALWAYS_INLINE std::uint32_t parse8_swar(std::uint64_t v) noexcept {
  v = (v & 0x0F0F0F0F0F0F0F0Full) * 2561 >> 8;
  v = (v & 0x00FF00FF00FF00FFull) * 6553601 >> 16;
  return static_cast<std::uint32_t>((v & 0x0000FFFF0000FFFFull) * 42949672960001ull >> 32);
}

// Parse 1..8 digits by left-padding with '0' into one word.
QM_ALWAYS_INLINE bool parse_upto8(const char* p, std::size_t n, std::uint32_t& out) noexcept {
  std::uint64_t v = kAsciiZeros8;
  std::memcpy(reinterpret_cast<char*>(&v) + (8 - n), p, n);
  if (QM_UNLIKELY(!all_digits8(v))) return false;
  out = parse8_swar(v);
  return true;
}

// Parse exactly 16 digits from a 16-byte buffer.
QM_ALWAYS_INLINE bool parse16(const char* buf, std::uint64_t& out) noexcept {
#if defined(QM_SIMD_SSE41)
  const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf));
  const __m128i d = _mm_sub_epi8(raw, _mm_set1_epi8('0'));
  const __m128i bad = _mm_or_si128(_mm_cmpgt_epi8(d, _mm_set1_epi8(9)),
                                   _mm_cmplt_epi8(d, _mm_setzero_si128()));
  if (QM_UNLIKELY(_mm_movemask_epi8(bad) != 0)) return false;
  const __m128i t1 = _mm_maddubs_epi16(d, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1,
                                                         10, 1, 10, 1));
  const __m128i t2 = _mm_madd_epi16(t1, _mm_setr_epi16(100, 1, 100, 1, 100, 1, 100, 1));
  const __m128i t3 = _mm_packus_epi32(t2, t2);
  const __m128i t4 = _mm_madd_epi16(t3, _mm_setr_epi16(10000, 1, 10000, 1, 10000, 1, 10000, 1));
  const auto hi = static_cast<std::uint32_t>(_mm_cvtsi128_si32(t4));
  const auto lo = static_cast<std::uint32_t>(_mm_extract_epi32(t4, 1));
  out = static_cast<std::uint64_t>(hi) * 100'000'000ull + lo;
  return true;
#elif defined(QM_SIMD_NEON)
  const uint8x16_t d = vsubq_u8(vld1q_u8(reinterpret_cast<const std::uint8_t*>(buf)),
                                vdupq_n_u8('0'));
  if (QM_UNLIKELY(vmaxvq_u8(d) > 9)) return false;
  const uint8x16_t m10 = {10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1};
  const uint16x8_t t1 = vpaddlq_u8(vmulq_u8(d, m10));                 // 8 x 2 digits
  const uint16x8_t m100 = {100, 1, 100, 1, 100, 1, 100, 1};
  const uint32x4_t t2 = vpaddlq_u16(vmulq_u16(t1, m100));             // 4 x 4 digits
  const uint32x4_t m10k = {10000, 1, 10000, 1};
  const uint64x2_t t3 = vpaddlq_u32(vmulq_u32(t2, m10k));             // 2 x 8 digits
  out = vgetq_lane_u64(t3, 0) * 100'000'000ull + vgetq_lane_u64(t3, 1);
  return true;
#else
  std::uint64_t a = 0, b = 0;
  std::memcpy(&a, buf, 8);
  std::memcpy(&b, buf + 8, 8);
  if (QM_UNLIKELY(!all_digits8(a) || !all_digits8(b))) return false;
  out = static_cast<std::uint64_t>(parse8_swar(a)) * 100'000'000ull + parse8_swar(b);
  return true;
#endif
}

// Unsigned decimal of 1..19 digits; same contract as scalar::parse_uint.
QM_ALWAYS_INLINE bool parse_uint(const char* p, std::size_t n, std::uint64_t& out) noexcept {
  if (QM_UNLIKELY(n == 0 || n > 19)) return false;
  if (n <= 8) {
    std::uint32_t v = 0;
    if (!parse_upto8(p, n, v)) return false;
    out = v;
    return true;
  }
  alignas(16) char buf[16];
  if (n <= 16) {
    std::memset(buf, '0', 16);
    std::memcpy(buf + (16 - n), p, n);
    return parse16(buf, out);
  }
  const std::size_t head = n - 16;  // 1..3 leading digits
  std::uint32_t h = 0;
  if (!parse_upto8(p, head, h)) return false;
  std::memcpy(buf, p + head, 16);
  std::uint64_t tail = 0;
  if (!parse16(buf, tail)) return false;
  out = static_cast<std::uint64_t>(h) * 10'000'000'000'000'000ull + tail;
  return true;
}

// Fixed-point price with up to 4 decimals; same contract as scalar::parse_price.
QM_ALWAYS_INLINE bool parse_price(const char* p, std::size_t n, Price& out) noexcept {
  if (QM_UNLIKELY(n == 0)) return false;
  const void* dotp = std::memchr(p, '.', n);
  const std::size_t dot = dotp ? static_cast<std::size_t>(static_cast<const char*>(dotp) - p) : n;
  const std::size_t frac_len = dot == n ? 0 : n - dot - 1;
  if (QM_UNLIKELY(dot == 0 || dot > 14)) return false;
  if (dot != n && QM_UNLIKELY(frac_len == 0 || frac_len > static_cast<std::size_t>(kPriceDecimals)))
    return false;

  std::uint64_t whole = 0;
  if (!parse_uint(p, dot, whole)) return false;

  std::uint32_t frac = 0;
  if (frac_len > 0) {
    // Place the fraction at bytes [4, 4+frac_len) of a '0'-filled word: right-pads to 4 digits.
    std::uint64_t v = kAsciiZeros8;
    std::memcpy(reinterpret_cast<char*>(&v) + 4, p + dot + 1, frac_len);
    if (QM_UNLIKELY(!all_digits8(v))) return false;
    frac = parse8_swar(v);
  }
  out = static_cast<Price>(whole) * kPriceScale + static_cast<Price>(frac);
  return true;
}

}  // namespace qm::simd
