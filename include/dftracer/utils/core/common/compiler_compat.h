#ifndef DFTRACER_UTILS_CORE_COMMON_COMPILER_COMPAT_H
#define DFTRACER_UTILS_CORE_COMMON_COMPILER_COMPAT_H

// Keep printf argument checks on compilers that support GNU format attributes.
#if defined(__GNUC__) || defined(__clang__)
#define DFTRACER_UTILS_PRINTF_FORMAT(format_index, first_arg) \
    __attribute__((__format__(__printf__, format_index, first_arg)))
#else
#define DFTRACER_UTILS_PRINTF_FORMAT(format_index, first_arg)
#endif

#endif  // DFTRACER_UTILS_CORE_COMMON_COMPILER_COMPAT_H
