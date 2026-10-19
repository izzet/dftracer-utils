#ifndef DFTRACER_UTILS_CORE_COMMON_COMPILER_COMPAT_H
#define DFTRACER_UTILS_CORE_COMMON_COMPILER_COMPAT_H

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <xmmintrin.h>
#define DFTRACER_UTILS_PREFETCH(address, writing, locality) \
    _mm_prefetch(reinterpret_cast<const char*>(address),    \
                 (locality) == 0   ? _MM_HINT_NTA           \
                 : (locality) == 1 ? _MM_HINT_T2            \
                 : (locality) == 2 ? _MM_HINT_T1            \
                                   : _MM_HINT_T0)
#elif defined(__GNUC__) || defined(__clang__)
#define DFTRACER_UTILS_PREFETCH(address, writing, locality) \
    __builtin_prefetch(address, writing, locality)
#else
#define DFTRACER_UTILS_PREFETCH(address, writing, locality) ((void)(address))
#endif

// Keep printf argument checks on compilers that support GNU format attributes.
#if defined(__GNUC__) || defined(__clang__)
#define DFTRACER_UTILS_PRINTF_FORMAT(format_index, first_arg) \
    __attribute__((__format__(__printf__, format_index, first_arg)))
#else
#define DFTRACER_UTILS_PRINTF_FORMAT(format_index, first_arg)
#endif

#if defined(_MSC_VER)
#define DFTRACER_UTILS_NOINLINE __declspec(noinline)
#define DFTRACER_UTILS_FORCE_INLINE __forceinline
#define DFTRACER_UTILS_COLD
#elif defined(__GNUC__) || defined(__clang__)
#define DFTRACER_UTILS_NOINLINE __attribute__((noinline))
#define DFTRACER_UTILS_FORCE_INLINE __attribute__((always_inline)) inline
#define DFTRACER_UTILS_COLD __attribute__((cold))
#else
#define DFTRACER_UTILS_NOINLINE
#define DFTRACER_UTILS_FORCE_INLINE inline
#define DFTRACER_UTILS_COLD
#endif

#endif  // DFTRACER_UTILS_CORE_COMMON_COMPILER_COMPAT_H
