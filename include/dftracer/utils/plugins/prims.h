#ifndef DFTRACER_UTILS_PLUGINS_PRIMS_H
#define DFTRACER_UTILS_PLUGINS_PRIMS_H

/** @file
 * Header-only numeric primitives for plugin and raw JIT bodies: bit counting,
 * log2 bucketing, power-of-two rounding, integer sqrt/gcd, alignment, and fast
 * integer hashing. Include alongside abi.h. The JIT layer lowers jit.ilog2 and
 * friends to these.
 */

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- bit operations (unsigned) ---- */

/** Count leading zero bits of @p x; 64 when @p x is 0. */
static inline int dftu_clz_u64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return x == 0 ? 64 : __builtin_clzll(x);
#else
    int n = 0;
    if (x == 0) return 64;
    if ((x >> 32) == 0) {
        n += 32;
        x <<= 32;
    }
    if ((x >> 48) == 0) {
        n += 16;
        x <<= 16;
    }
    if ((x >> 56) == 0) {
        n += 8;
        x <<= 8;
    }
    if ((x >> 60) == 0) {
        n += 4;
        x <<= 4;
    }
    if ((x >> 62) == 0) {
        n += 2;
        x <<= 2;
    }
    return n + ((x >> 63) == 0);
#endif
}

/** Count trailing zero bits of @p x; 64 when @p x is 0. */
static inline int dftu_ctz_u64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return x == 0 ? 64 : __builtin_ctzll(x);
#else
    int n = 0;
    if (x == 0) return 64;
    if ((x & UINT64_C(0xffffffff)) == 0) {
        n += 32;
        x >>= 32;
    }
    if ((x & UINT64_C(0xffff)) == 0) {
        n += 16;
        x >>= 16;
    }
    if ((x & UINT64_C(0xff)) == 0) {
        n += 8;
        x >>= 8;
    }
    if ((x & UINT64_C(0xf)) == 0) {
        n += 4;
        x >>= 4;
    }
    if ((x & UINT64_C(0x3)) == 0) {
        n += 2;
        x >>= 2;
    }
    return n + ((x & 1) == 0);
#endif
}

/** Number of set bits in @p x. */
static inline int dftu_popcount_u64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    x -= (x >> 1) & UINT64_C(0x5555555555555555);
    x = (x & UINT64_C(0x3333333333333333)) +
        ((x >> 2) & UINT64_C(0x3333333333333333));
    x = (x + (x >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    return (int)((x * UINT64_C(0x0101010101010101)) >> 56);
#endif
}

/** Floor of log2(@p x): index of the highest set bit; 0 when @p x is 0. */
static inline int dftu_ilog2_u64(uint64_t x) {
    return x == 0 ? 0 : 63 - dftu_clz_u64(x);
}

/** Number of bits needed to represent @p x (``ilog2 + 1``); 0 when @p x is 0.
 */
static inline int dftu_bit_width_u64(uint64_t x) {
    return x == 0 ? 0 : 64 - dftu_clz_u64(x);
}

/** Smallest power of two >= @p x (1 when @p x is 0). Saturates to 0 above
 * 2^63. */
static inline uint64_t dftu_ceil_pow2_u64(uint64_t x) {
    if (x <= 1) return 1;
    return (uint64_t)2 << (63 - dftu_clz_u64(x - 1));
}

/** Largest power of two <= @p x; 0 when @p x is 0. */
static inline uint64_t dftu_floor_pow2_u64(uint64_t x) {
    return x == 0 ? 0 : (uint64_t)1 << (63 - dftu_clz_u64(x));
}

/** Rotate @p x left by @p r bits (@p r taken mod 64). */
static inline uint64_t dftu_rotl_u64(uint64_t x, uint64_t r) {
    r &= 63;
    return r == 0 ? x : (x << r) | (x >> (64 - r));
}

/** Rotate @p x right by @p r bits (@p r taken mod 64). */
static inline uint64_t dftu_rotr_u64(uint64_t x, uint64_t r) {
    r &= 63;
    return r == 0 ? x : (x >> r) | (x << (64 - r));
}

/* ---- integer math ---- */

static inline int64_t dftu_abs_i64(int64_t x) { return x < 0 ? -x : x; }
static inline int64_t dftu_min_i64(int64_t a, int64_t b) {
    return a < b ? a : b;
}
static inline int64_t dftu_max_i64(int64_t a, int64_t b) {
    return a > b ? a : b;
}

/** Clamp @p x to the inclusive range [@p lo, @p hi]. */
static inline int64_t dftu_clamp_i64(int64_t x, int64_t lo, int64_t hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/** Ceiling of @p a / @p b (integer division rounding up); 0 when @p b is 0. */
static inline int64_t dftu_div_ceil_i64(int64_t a, int64_t b) {
    if (b == 0) return 0;
    return (a + (b - 1)) / b;
}

/** @p a / @p b rounded to nearest, half away from zero; 0 when @p b is 0. */
static inline int64_t dftu_div_round_i64(int64_t a, int64_t b) {
    if (b == 0) return 0;
    int64_t h = (b < 0 ? -b : b) / 2;
    return ((a < 0) == (b < 0)) ? (a + h) / b : (a - h) / b;
}

/** Round @p x up to the next multiple of @p a; @p x when @p a is 0. */
static inline uint64_t dftu_align_up_u64(uint64_t x, uint64_t a) {
    if (a == 0) return x;
    return ((x + a - 1) / a) * a;
}

/** Round @p x down to the previous multiple of @p a; @p x when @p a is 0. */
static inline uint64_t dftu_align_down_u64(uint64_t x, uint64_t a) {
    return a == 0 ? x : (x / a) * a;
}

/** Integer square root: the largest @c r with ``r*r <= x``. */
static inline uint64_t dftu_isqrt_u64(uint64_t x) {
    if (x == 0) return 0;
    uint64_t r = (uint64_t)sqrt((double)x);
    while (r > 0 && r > x / r) --r;     /* correct any float rounding up */
    while ((r + 1) <= x / (r + 1)) ++r; /* and up */
    return r;
}

/** Greatest common divisor of @p a and @p b (0 only when both are 0). */
static inline uint64_t dftu_gcd_u64(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* ---- fast integer hashing ---- */

/** High 64 bits of the 128-bit product @p a * @p b. */
static inline uint64_t dftu_mul_hi_u64(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    return (uint64_t)((__extension__((unsigned __int128)a *
                                     (unsigned __int128)b)) >>
                      64);
#else
    const uint64_t mask = UINT64_C(0xffffffff);
    const uint64_t low = (a & mask) * (b & mask);
    const uint64_t middle = (a >> 32) * (b & mask) + (low >> 32);
    const uint64_t carry = (a & mask) * (b >> 32) + (middle & mask);
    return (a >> 32) * (b >> 32) + (middle >> 32) + (carry >> 32);
#endif
}

/** SplitMix64 finalizer: a fast, well-avalanched hash of @p x.
 *
 * The finalizer from Sebastiano Vigna's SplitMix64 generator (public domain /
 * CC0): https://prng.di.unimi.it/splitmix64.c
 */
static inline uint64_t dftu_mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/** Map @p x uniformly into ``[0, n)`` without a modulo; 0 when @p n is 0.
 *
 * Daniel Lemire's multiply-shift range reduction ("A fast alternative to the
 * modulo reduction", 2016): https://lemire.me/blog/2016/06/30/
 */
static inline uint64_t dftu_fastrange_u64(uint64_t x, uint64_t n) {
    return n == 0 ? 0 : dftu_mul_hi_u64(x, n);
}

/* ---- float helpers ---- */

static inline double dftu_min_f64(double a, double b) { return a < b ? a : b; }
static inline double dftu_max_f64(double a, double b) { return a > b ? a : b; }

/** Clamp @p x to the inclusive range [@p lo, @p hi]. */
static inline double dftu_clamp_f64(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline double dftu_sqrt_f64(double x) { return sqrt(x); }
static inline double dftu_log2_f64(double x) { return log2(x); }
static inline double dftu_log_f64(double x) { return log(x); }
static inline double dftu_exp_f64(double x) { return exp(x); }

/** Fused multiply-add ``a*b + c`` with a single rounding. */
static inline double dftu_fma_f64(double a, double b, double c) {
    return fma(a, b, c);
}

/** Linear interpolation from @p a to @p b by @p t (unclamped). */
static inline double dftu_lerp_f64(double a, double b, double t) {
    return fma(t, b - a, a);
}

/** Magnitude of @p x with the sign of @p y. */
static inline double dftu_copysign_f64(double x, double y) {
    return copysign(x, y);
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DFTRACER_UTILS_PLUGINS_PRIMS_H
