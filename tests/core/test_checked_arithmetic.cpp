#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/checked_arithmetic.h>
#include <dftracer/utils/core/common/int128.h>
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace {
template <class T>
void check(T a, T b) {
    using Wide =
        std::conditional_t<std::is_signed_v<T>, dftracer::utils::int128_t,
                           dftracer::utils::uint128_t>;
    const Wide sum = static_cast<Wide>(a) + static_cast<Wide>(b);
    const Wide product = static_cast<Wide>(a) * static_cast<Wide>(b);
    const auto outside = [](Wide value) {
        return value < static_cast<Wide>(std::numeric_limits<T>::min()) ||
               value > static_cast<Wide>(std::numeric_limits<T>::max());
    };
    T result{};
    CHECK(dftracer::utils::add_overflow(a, b, &result) == outside(sum));
    CHECK(result == static_cast<T>(sum));
    CHECK(dftracer::utils::mul_overflow(a, b, &result) == outside(product));
    CHECK(result == static_cast<T>(product));
    // The output may alias either operand, as it does in FieldStat and sums.
    T alias = a;
    CHECK(dftracer::utils::add_overflow(alias, b, &alias) == outside(sum));
    CHECK(alias == static_cast<T>(sum));
}

template <class T>
void boundaries() {
    constexpr auto MIN = std::numeric_limits<T>::min();
    constexpr auto MAX = std::numeric_limits<T>::max();
    const std::array<T, 7> values{MIN,  static_cast<T>(MIN + 1), T{0}, T{1},
                                  T{2}, static_cast<T>(MAX - 1), MAX};
    for (T a : values)
        for (T b : values) check(a, b);
    if constexpr (std::is_signed_v<T>) {
        check(MIN, T{-1});
        check(T{-1}, MIN);
        check(T{-1}, T{-1});
    }
}
}  // namespace

TEST_CASE(
    "checked arithmetic stores wrapped values and detects range failures") {
    boundaries<std::int8_t>();
    boundaries<std::uint8_t>();
    boundaries<std::int16_t>();
    boundaries<std::uint16_t>();
    boundaries<std::int32_t>();
    boundaries<std::uint32_t>();
    boundaries<std::int64_t>();
    boundaries<std::uint64_t>();
    std::uint64_t state = 0x42f3981c8b9ade71;
    const auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (int i = 0; i < 2000; ++i) {
        const auto a = next(), b = next();
        check(a, b);
        check(static_cast<std::int64_t>(a), static_cast<std::int64_t>(b));
        check(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
        check(static_cast<std::int32_t>(a), static_cast<std::int32_t>(b));
    }
}
