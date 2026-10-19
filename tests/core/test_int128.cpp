#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/calendar.h>
#include <dftracer/utils/core/common/int128.h>
#include <dftracer/utils/dataframe/internal/decimal_arith.h>
#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <limits>

using PortableInt = dftracer::utils::detail::Int128<true>;
using PortableUint = dftracer::utils::detail::Int128<false>;
namespace df = dftracer::utils::dataframe;

TEST_CASE("portable 128-bit arithmetic preserves two-word boundaries") {
    constexpr PortableUint WORD = PortableUint{1} << 64;
    constexpr auto MAX = ~PortableUint{0};
    static_assert((WORD - 1) + 1 == WORD);
    static_assert((WORD - 1) * (WORD - 1) == (MAX - 2 * WORD + 2));
    static_assert((WORD + 7) / 3 == WORD / 3 + 2);
    static_assert((WORD + 7) % 3 == 2);
    static_assert(PortableInt{-17} / 5 == -3);
    static_assert(PortableInt{-17} % 5 == -2);
    static_assert(PortableInt{-1} >> 127 == -1);
    CHECK(MAX + 1 == 0);
    CHECK((WORD >> 64) == 1);
    CHECK((MAX / (WORD + 1)) == WORD - 1);
    CHECK((MAX % (WORD + 1)) == 0);
    CHECK(PortableInt{-1} < 0);
    CHECK(PortableInt{std::numeric_limits<std::uint64_t>::max()} > 0);
    CHECK(static_cast<std::int64_t>(PortableInt{-123}) == -123);
    CHECK(static_cast<double>(PortableInt{-123}) == -123.0);
    CHECK(static_cast<bool>(WORD));
    CHECK_FALSE(static_cast<bool>(PortableUint{}));
    const PortableInt negative{-2};
    std::array<std::uint64_t, 2> bytes{};
    std::memcpy(bytes.data(), &negative, sizeof(negative));
    CHECK(bytes[0] == std::numeric_limits<std::uint64_t>::max() - 1);
    CHECK(bytes[1] == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("decimal128 and decimal256 retain exact arithmetic and byte layout") {
    using dftracer::utils::int128_t;
    using dftracer::utils::uint128_t;
    const auto power = df::pow10_u128(38);
    CHECK(power / 10 == df::pow10_u128(37));
    CHECK(power % 10 == 0);
    const auto value = -static_cast<int128_t>(power - 1);
    std::array<char, 16> bytes{};
    df::store_i128(bytes.data(), value);
    CHECK(df::load_i128(bytes.data()) == value);
    CHECK(df::i128_fits_precision(value, 38));
    CHECK_FALSE(df::i128_fits_precision(value, 37));
    const auto rescaled = df::rescale_up_i128(-123, 0, 10);
    REQUIRE(rescaled.has_value());
    CHECK(*rescaled == -int128_t{123} * 10000000000LL);
    CHECK_FALSE(df::rescale_up_i128(value, 0, 1).has_value());
    const auto product =
        df::mul_checked_i128(int128_t{123456789} * 1000000000, -987654321, 38);
    REQUIRE(product.has_value());
    CHECK(*product / 1000000000 == -int128_t{121932631112635269LL});
    const auto divided = df::muldiv_checked_i128(-123456, 4, 100, 38);
    REQUIRE(divided.has_value());
    CHECK(*divided == -12345600);
    const auto wide = df::umul128_256(~uint128_t{0}, ~uint128_t{0});
    CHECK(wide.lo == 1);
    CHECK(wide.hi == ~uint128_t{1});
    const auto limbs = df::pow10_256(76);
    CHECK(df::limbs256_fits_precision(limbs, 77));
    CHECK_FALSE(df::limbs256_fits_precision(limbs, 76));
}

TEST_CASE(
    "calendar arithmetic handles negative epochs and wide intermediates") {
    using namespace dftracer::utils;
    const auto before = civil_time(-1, 1);
    CHECK(before.year == 1969);
    CHECK(before.month == 12);
    CHECK(before.day == 31);
    CHECK(before.hour == 23);
    CHECK(before.nanosecond == 999999999);
    const auto huge =
        civil_time(std::numeric_limits<std::int64_t>::max(), NS_PER_SECOND);
    CHECK(huge.epoch_seconds == std::numeric_limits<std::int64_t>::max());
    std::string text;
    calendar_detail::append_wide(
        text,
        -static_cast<CalendarWide>(std::numeric_limits<std::uint64_t>::max()));
    CHECK(text == "-18446744073709551615");
}

#if defined(__SIZEOF_INT128__)
namespace {
std::uint64_t random_word(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

template <class Native, class Portable>
void check_bits(Native native, Portable portable) {
    // Compare numerical words so the oracle also works on big-endian hosts;
    // the fallback's low-word-first byte layout is needed only on Windows.
    const auto bits = static_cast<dftracer::utils::uint128_t>(native);
    const PortableUint actual(portable);
    CHECK(static_cast<std::uint64_t>(actual) ==
          static_cast<std::uint64_t>(bits));
    CHECK(static_cast<std::uint64_t>(actual >> 64) ==
          static_cast<std::uint64_t>(bits >> 64));
}
}  // namespace

TEST_CASE(
    "portable arithmetic agrees with native int128 on randomized operands") {
    using dftracer::utils::int128_t;
    using dftracer::utils::uint128_t;
    std::uint64_t state = 0x8b5ad4cef9732611;
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const auto low_a = random_word(state);
        const auto high_a = random_word(state);
        const auto low_b = random_word(state);
        const auto high_b = random_word(state);
        const PortableUint a = (PortableUint{high_a} << 64) | low_a;
        const PortableUint b = (PortableUint{high_b} << 64) | low_b;
        const uint128_t na = (uint128_t{high_a} << 64) | low_a;
        const uint128_t nb = (uint128_t{high_b} << 64) | low_b;
        check_bits(na + nb, a + b);
        check_bits(na - nb, a - b);
        check_bits(na * nb, a * b);
        check_bits(na / nb, a / b);
        check_bits(na % nb, a % b);
        check_bits(na / low_b, a / low_b);
        check_bits(na % low_b, a % low_b);
        check_bits(na & nb, a & b);
        check_bits(na | nb, a | b);
        check_bits(na ^ nb, a ^ b);
        const int shift = static_cast<int>(random_word(state) % 128);
        check_bits(na << shift, a << shift);
        check_bits(na >> shift, a >> shift);
        CHECK((a < b) == (na < nb));
        CHECK(static_cast<double>(a) == static_cast<double>(na));
        CHECK(static_cast<float>(a) == static_cast<float>(na));
        const PortableInt sa(a), sb(b);
        const auto nsa = static_cast<int128_t>(na);
        const auto nsb = static_cast<int128_t>(nb);
        check_bits(na + nb, sa + sb);
        check_bits(na - nb, sa - sb);
        check_bits(na * nb, sa * sb);
        check_bits(nsa / nsb, sa / sb);
        check_bits(nsa % nsb, sa % sb);
        check_bits(nsa >> shift, sa >> shift);
        CHECK((sa < sb) == (nsa < nsb));
        CHECK(static_cast<double>(sa) == static_cast<double>(nsa));
    }
}
#endif
