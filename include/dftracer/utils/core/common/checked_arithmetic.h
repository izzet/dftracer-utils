#ifndef DFTRACER_UTILS_CORE_COMMON_CHECKED_ARITHMETIC_H
#define DFTRACER_UTILS_CORE_COMMON_CHECKED_ARITHMETIC_H

#include <bit>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dftracer::utils {

// Like the compiler overflow builtins, always store the wrapped result and
// return whether the exact mathematical result is outside T's range.
template <std::integral T>
constexpr bool add_overflow(std::type_identity_t<T> a,
                            std::type_identity_t<T> b, T* result) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, result);
#else
    using U = std::make_unsigned_t<T>;
    const U bits = static_cast<U>(static_cast<U>(a) + static_cast<U>(b));
    if constexpr (std::is_signed_v<T>) {
        *result = std::bit_cast<T>(bits);
        return (a < 0) == (b < 0) && (*result < 0) != (a < 0);
    } else {
        *result = bits;
        return bits < a;
    }
#endif
}

template <std::integral T>
constexpr bool mul_overflow(std::type_identity_t<T> a,
                            std::type_identity_t<T> b, T* result) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, result);
#else
    using U = std::make_unsigned_t<T>;
    const U bits = static_cast<U>(
        static_cast<std::uintmax_t>(static_cast<U>(a)) * static_cast<U>(b));
    if constexpr (std::is_signed_v<T>) {
        *result = std::bit_cast<T>(bits);
        constexpr T MAX = std::numeric_limits<T>::max();
        constexpr T MIN = std::numeric_limits<T>::min();
        if (a > 0) return b > 0 ? a > MAX / b : b < MIN / a;
        if (a < 0) return b > 0 ? a < MIN / b : b < 0 && a < MAX / b;
        return false;
    } else {
        *result = bits;
        return b != 0 && a > std::numeric_limits<T>::max() / b;
    }
#endif
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_CHECKED_ARITHMETIC_H
