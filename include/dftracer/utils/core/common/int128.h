#ifndef DFTRACER_UTILS_CORE_COMMON_INT128_H
#define DFTRACER_UTILS_CORE_COMMON_INT128_H

#include <bit>
#include <compare>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <type_traits>

namespace dftracer::utils {

namespace detail {

// Fixed-width two's-complement storage, also used directly by the differential
// tests on compilers with native 128-bit arithmetic. Low word first matches
// the decimal byte layout on Windows; no sign word or heap allocation.
template <bool Signed>
class alignas(16) Int128 {
    template <bool>
    friend class Int128;

    std::uint64_t low_ = 0;
    std::uint64_t high_ = 0;

    static constexpr Int128 words(std::uint64_t low,
                                  std::uint64_t high) noexcept {
        Int128 result;
        result.low_ = low;
        result.high_ = high;
        return result;
    }
    constexpr bool negative() const noexcept {
        return Signed && (high_ >> 63) != 0;
    }

    static constexpr Int128 multiply64(std::uint64_t a,
                                       std::uint64_t b) noexcept {
        constexpr std::uint64_t MASK = 0xffffffff;
        const auto low = (a & MASK) * (b & MASK);
        const auto middle = (a >> 32) * (b & MASK) + (low >> 32);
        const auto carry = (a & MASK) * (b >> 32) + (middle & MASK);
        return words((carry << 32) | (low & MASK),
                     (a >> 32) * (b >> 32) + (middle >> 32) + (carry >> 32));
    }

    static constexpr Int128 divide(Int128 a, Int128 b,
                                   Int128& remainder) noexcept {
        using Unsigned = Int128<false>;
        if (!static_cast<bool>(b))
            std::abort();  // division by zero, as for native integers
        const bool negative_quotient = a.negative() != b.negative();
        const bool negative_remainder = a.negative();
        const Unsigned dividend(a.negative() ? -a : a);
        const Unsigned divisor(b.negative() ? -b : b);
        Unsigned quotient;
        Unsigned rest;
        if (dividend.high_ == 0 && divisor.high_ == 0) {
            quotient = dividend.low_ / divisor.low_;
            rest = dividend.low_ % divisor.low_;
        } else {
            // Restoring division. The carry records the 129th bit before a
            // subtraction, so divisors with their top bit set work as well.
            const int bits = dividend.high_ != 0
                                 ? 128 - std::countl_zero(dividend.high_)
                                 : 64 - std::countl_zero(dividend.low_);
            for (int bit = bits - 1; bit >= 0; --bit) {
                const bool carry = (rest.high_ >> 63) != 0;
                rest <<= 1;
                rest.low_ |= bit >= 64 ? (dividend.high_ >> (bit - 64)) & 1
                                       : (dividend.low_ >> bit) & 1;
                if (carry || rest >= divisor) {
                    rest -= divisor;
                    if (bit >= 64)
                        quotient.high_ |= std::uint64_t{1} << (bit - 64);
                    else
                        quotient.low_ |= std::uint64_t{1} << bit;
                }
            }
        }
        remainder = Int128(rest);
        if (negative_remainder) remainder = -remainder;
        const Int128 result(quotient);
        return negative_quotient ? -result : result;
    }

   public:
    constexpr Int128() noexcept = default;
    template <std::integral T>
    constexpr Int128(T value) noexcept
        : low_(static_cast<std::uint64_t>(value)) {
        if constexpr (std::is_signed_v<T>)
            if (value < 0) high_ = std::numeric_limits<std::uint64_t>::max();
    }
    template <bool OtherSigned>
    explicit constexpr Int128(Int128<OtherSigned> value) noexcept
        : low_(value.low_), high_(value.high_) {}

    template <std::integral T>
    explicit constexpr operator T() const noexcept {
        if constexpr (std::is_same_v<T, bool>)
            return (low_ | high_) != 0;
        else
            return static_cast<T>(low_);
    }
    template <std::floating_point T>
    explicit constexpr operator T() const noexcept {
        using Unsigned = Int128<false>;
        Unsigned magnitude(negative() ? -(*this) : *this);
        const int bits = magnitude.high_ != 0
                             ? 128 - std::countl_zero(magnitude.high_)
                             : 64 - std::countl_zero(magnitude.low_);
        constexpr int DIGITS = std::numeric_limits<T>::digits;
        const int shift = bits > DIGITS ? bits - DIGITS : 0;
        if (shift > 0) {
            const Unsigned mask = (Unsigned{1} << shift) - 1;
            const Unsigned lost = magnitude & mask;
            magnitude >>= shift;
            const Unsigned halfway = Unsigned{1} << (shift - 1);
            if (lost > halfway || (lost == halfway && (magnitude.low_ & 1)))
                ++magnitude;
        }
        // After rounding the retained bits are exactly representable in T.
        T result = static_cast<T>(magnitude.high_) *
                       static_cast<T>(18446744073709551616.0L) +
                   static_cast<T>(magnitude.low_);
        for (int i = 0; i < shift; ++i) result *= 2;
        return negative() ? -result : result;
    }

    friend constexpr bool operator==(Int128, Int128) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(Int128 a,
                                                      Int128 b) noexcept {
        if (a.negative() != b.negative())
            return a.negative() ? std::strong_ordering::less
                                : std::strong_ordering::greater;
        if (a.high_ != b.high_) return a.high_ <=> b.high_;
        return a.low_ <=> b.low_;
    }
    constexpr Int128 operator+() const noexcept { return *this; }
    constexpr Int128 operator~() const noexcept { return words(~low_, ~high_); }
    constexpr Int128 operator-() const noexcept { return ~(*this) + Int128{1}; }
    friend constexpr Int128 operator+(Int128 a, Int128 b) noexcept {
        const auto low = a.low_ + b.low_;
        return words(low, a.high_ + b.high_ + (low < a.low_));
    }
    friend constexpr Int128 operator-(Int128 a, Int128 b) noexcept {
        return words(a.low_ - b.low_, a.high_ - b.high_ - (a.low_ < b.low_));
    }
    friend constexpr Int128 operator*(Int128 a, Int128 b) noexcept {
        auto result = multiply64(a.low_, b.low_);
        result.high_ += a.low_ * b.high_ + a.high_ * b.low_;
        return result;
    }
    friend constexpr Int128 operator/(Int128 a, Int128 b) noexcept {
        Int128 remainder;
        return divide(a, b, remainder);
    }
    friend constexpr Int128 operator%(Int128 a, Int128 b) noexcept {
        Int128 remainder;
        divide(a, b, remainder);
        return remainder;
    }
    friend constexpr Int128 operator&(Int128 a, Int128 b) noexcept {
        return words(a.low_ & b.low_, a.high_ & b.high_);
    }
    friend constexpr Int128 operator|(Int128 a, Int128 b) noexcept {
        return words(a.low_ | b.low_, a.high_ | b.high_);
    }
    friend constexpr Int128 operator^(Int128 a, Int128 b) noexcept {
        return words(a.low_ ^ b.low_, a.high_ ^ b.high_);
    }
    friend constexpr Int128 operator<<(Int128 a, int shift) noexcept {
        if (shift <= 0) return a;
        if (shift >= 128) return {};
        if (shift >= 64) return words(0, a.low_ << (shift - 64));
        return words(a.low_ << shift,
                     (a.high_ << shift) | (a.low_ >> (64 - shift)));
    }
    friend constexpr Int128 operator>>(Int128 a, int shift) noexcept {
        if (shift <= 0) return a;
        const auto fill =
            a.negative() ? std::numeric_limits<std::uint64_t>::max() : 0;
        if (shift >= 128) return words(fill, fill);
        if (shift >= 64) {
            const int rest = shift - 64;
            return words(
                rest == 0 ? a.high_ : (a.high_ >> rest) | (fill << (64 - rest)),
                fill);
        }
        return words((a.low_ >> shift) | (a.high_ << (64 - shift)),
                     (a.high_ >> shift) | (fill << (64 - shift)));
    }

    constexpr Int128& operator+=(Int128 b) noexcept {
        return *this = *this + b;
    }
    constexpr Int128& operator-=(Int128 b) noexcept {
        return *this = *this - b;
    }
    constexpr Int128& operator*=(Int128 b) noexcept {
        return *this = *this * b;
    }
    constexpr Int128& operator/=(Int128 b) noexcept {
        return *this = *this / b;
    }
    constexpr Int128& operator%=(Int128 b) noexcept {
        return *this = *this % b;
    }
    constexpr Int128& operator&=(Int128 b) noexcept {
        return *this = *this & b;
    }
    constexpr Int128& operator|=(Int128 b) noexcept {
        return *this = *this | b;
    }
    constexpr Int128& operator^=(Int128 b) noexcept {
        return *this = *this ^ b;
    }
    constexpr Int128& operator<<=(int shift) noexcept {
        return *this = *this << shift;
    }
    constexpr Int128& operator>>=(int shift) noexcept {
        return *this = *this >> shift;
    }
    constexpr Int128& operator++() noexcept { return *this += 1; }
    constexpr Int128& operator--() noexcept { return *this -= 1; }
    constexpr Int128 operator++(int) noexcept {
        auto old = *this;
        ++*this;
        return old;
    }
    constexpr Int128 operator--(int) noexcept {
        auto old = *this;
        --*this;
        return old;
    }
};

static_assert(sizeof(Int128<true>) == 16 && sizeof(Int128<false>) == 16);
static_assert(std::is_trivially_copyable_v<Int128<true>> &&
              std::is_trivially_copyable_v<Int128<false>>);
}  // namespace detail

#if defined(__SIZEOF_INT128__)
// Keep native code generation and ABI on GCC/Clang. `__extension__` suppresses
// their pedantic diagnostic; no extension is needed in MSVC's fallback.
__extension__ typedef __int128 int128_t;
__extension__ typedef unsigned __int128 uint128_t;
#else
static_assert(std::endian::native == std::endian::little,
              "portable int128 storage requires little-endian words");
using int128_t = detail::Int128<true>;
using uint128_t = detail::Int128<false>;
#endif

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_INT128_H
