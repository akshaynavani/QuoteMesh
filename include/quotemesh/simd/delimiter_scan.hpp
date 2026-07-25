#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "quotemesh/simd/arch.hpp"

// Structural-character scanning in 64-byte blocks, in the spirit of simdjson's stage 1:
// each block produces one 64-bit mask for field delimiters and one for line terminators,
// so the parser can walk structure with countr_zero instead of inspecting every byte.
namespace qm::simd {

inline constexpr std::size_t kBlockSize = 64;

struct BlockMasks {
  std::uint64_t delim;  // bit i set => block[i] == '|'
  std::uint64_t eol;    // bit i set => block[i] == '\n'
};

#if defined(QM_SIMD_AVX2)

QM_ALWAYS_INLINE BlockMasks scan_block64(const char* p) noexcept {
  const __m256i lo = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
  const __m256i hi = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 32));
  const __m256i pipe = _mm256_set1_epi8('|');
  const __m256i nl = _mm256_set1_epi8('\n');
  auto mask = [](__m256i a, __m256i b) -> std::uint64_t {
    const auto l = static_cast<std::uint32_t>(_mm256_movemask_epi8(a));
    const auto h = static_cast<std::uint32_t>(_mm256_movemask_epi8(b));
    return static_cast<std::uint64_t>(l) | (static_cast<std::uint64_t>(h) << 32);
  };
  return {mask(_mm256_cmpeq_epi8(lo, pipe), _mm256_cmpeq_epi8(hi, pipe)),
          mask(_mm256_cmpeq_epi8(lo, nl), _mm256_cmpeq_epi8(hi, nl))};
}

#elif defined(QM_SIMD_SSE2)

QM_ALWAYS_INLINE BlockMasks scan_block64(const char* p) noexcept {
  const __m128i pipe = _mm_set1_epi8('|');
  const __m128i nl = _mm_set1_epi8('\n');
  BlockMasks m{0, 0};
  for (int i = 0; i < 4; ++i) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * i));
    const auto d = static_cast<std::uint64_t>(static_cast<std::uint16_t>(
        _mm_movemask_epi8(_mm_cmpeq_epi8(v, pipe))));
    const auto e = static_cast<std::uint64_t>(static_cast<std::uint16_t>(
        _mm_movemask_epi8(_mm_cmpeq_epi8(v, nl))));
    m.delim |= d << (16 * i);
    m.eol |= e << (16 * i);
  }
  return m;
}

#elif defined(QM_SIMD_NEON)

// NEON has no movemask; fold 4 compare results into a 64-bit mask with pairwise adds.
QM_ALWAYS_INLINE std::uint64_t neon_mask64(uint8x16_t c0, uint8x16_t c1, uint8x16_t c2,
                                           uint8x16_t c3) noexcept {
  const uint8x16_t bits = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
                           0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
  uint8x16_t s0 = vpaddq_u8(vandq_u8(c0, bits), vandq_u8(c1, bits));
  uint8x16_t s1 = vpaddq_u8(vandq_u8(c2, bits), vandq_u8(c3, bits));
  s0 = vpaddq_u8(s0, s1);
  s0 = vpaddq_u8(s0, s0);
  return vgetq_lane_u64(vreinterpretq_u64_u8(s0), 0);
}

QM_ALWAYS_INLINE BlockMasks scan_block64(const char* p) noexcept {
  const auto* u = reinterpret_cast<const std::uint8_t*>(p);
  const uint8x16_t v0 = vld1q_u8(u), v1 = vld1q_u8(u + 16), v2 = vld1q_u8(u + 32),
                   v3 = vld1q_u8(u + 48);
  const uint8x16_t pipe = vdupq_n_u8('|');
  const uint8x16_t nl = vdupq_n_u8('\n');
  return {neon_mask64(vceqq_u8(v0, pipe), vceqq_u8(v1, pipe), vceqq_u8(v2, pipe),
                      vceqq_u8(v3, pipe)),
          neon_mask64(vceqq_u8(v0, nl), vceqq_u8(v1, nl), vceqq_u8(v2, nl), vceqq_u8(v3, nl))};
}

#else

QM_ALWAYS_INLINE BlockMasks scan_block64(const char* p) noexcept {
  BlockMasks m{0, 0};
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    m.delim |= static_cast<std::uint64_t>(p[i] == '|') << i;
    m.eol |= static_cast<std::uint64_t>(p[i] == '\n') << i;
  }
  return m;
}

#endif

// Scan a final partial block of `n` (< 64) bytes without reading past the buffer.
inline BlockMasks scan_tail(const char* p, std::size_t n) noexcept {
  alignas(64) char buf[kBlockSize];
  std::memset(buf, 0, sizeof buf);
  std::memcpy(buf, p, n);
  return scan_block64(buf);
}

}  // namespace qm::simd
