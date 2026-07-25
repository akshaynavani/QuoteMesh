#pragma once

// Compile-time SIMD backend selection. Build with -DQM_FORCE_SCALAR=1 to get the portable
// reference implementation everywhere (useful for differential testing and benchmarking).

#if defined(QM_FORCE_SCALAR)
#  define QM_SIMD_SCALAR 1
#elif defined(__AVX2__)
#  define QM_SIMD_AVX2 1
#  define QM_SIMD_SSE2 1
#  include <immintrin.h>
#elif defined(__SSE2__) || defined(_M_X64)
#  define QM_SIMD_SSE2 1
#  include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__aarch64__)
#  define QM_SIMD_NEON 1
#  include <arm_neon.h>
#else
#  define QM_SIMD_SCALAR 1
#endif

#if !defined(QM_FORCE_SCALAR) && defined(__SSE4_1__)
#  define QM_SIMD_SSE41 1
#  include <smmintrin.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define QM_ALWAYS_INLINE inline __attribute__((always_inline))
#  define QM_LIKELY(x) __builtin_expect(!!(x), 1)
#  define QM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define QM_ALWAYS_INLINE inline
#  define QM_LIKELY(x) (x)
#  define QM_UNLIKELY(x) (x)
#endif

namespace qm::simd {

constexpr const char* compiled_backend() noexcept {
#if defined(QM_SIMD_AVX2)
  return "avx2";
#elif defined(QM_SIMD_SSE2)
  return "sse2";
#elif defined(QM_SIMD_NEON)
  return "neon";
#else
  return "scalar";
#endif
}

}  // namespace qm::simd
