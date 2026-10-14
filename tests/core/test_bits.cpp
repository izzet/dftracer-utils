#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/bits.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>

using namespace dftracer::utils::bits;

TEST_CASE("zero has no set bits and a zero bit width") {
    CHECK(clz_u64(0) == 64);
    CHECK(ctz_u64(0) == 64);
    CHECK(popcount_u64(0) == 0);
    CHECK(ilog2_u64(0) == 0);
    CHECK(bit_width_u64(0) == 0);
}

TEST_CASE("single bits retain their positions across both 32-bit halves") {
    for (int bit = 0; bit < 64; ++bit) {
        CAPTURE(bit);
        const std::uint64_t value = std::uint64_t{1} << bit;
        CHECK(clz_u64(value) == 63 - bit);
        CHECK(ctz_u64(value) == bit);
        CHECK(popcount_u64(value) == 1);
        CHECK(ilog2_u64(value) == bit);
        CHECK(bit_width_u64(value) == bit + 1);
    }
}

TEST_CASE("dense and alternating patterns preserve all 64 bits") {
    const auto all = std::numeric_limits<std::uint64_t>::max();
    CHECK(clz_u64(all) == 0);
    CHECK(ctz_u64(all) == 0);
    CHECK(popcount_u64(all) == 64);
    CHECK(ilog2_u64(all) == 63);
    CHECK(bit_width_u64(all) == 64);
    CHECK(popcount_u64(0xaaaaaaaaaaaaaaaaULL) == 32);
    CHECK(ctz_u64(0xaaaaaaaaaaaaaaaaULL) == 1);
    CHECK(clz_u64(0x5555555555555555ULL) == 1);
}
