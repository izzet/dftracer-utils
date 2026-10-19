#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/bits.h>
#include <dftracer/utils/plugins/prims.h>
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

TEST_CASE("C plugin primitives agree with the C++ bit helpers") {
    std::uint64_t value = 0;
    for (int i = 0; i < 1024; ++i) {
        CHECK(dftu_clz_u64(value) == clz_u64(value));
        CHECK(dftu_ctz_u64(value) == ctz_u64(value));
        CHECK(dftu_popcount_u64(value) == popcount_u64(value));
        CHECK(dftu_ilog2_u64(value) == ilog2_u64(value));
        CHECK(dftu_bit_width_u64(value) == bit_width_u64(value));
        value = value * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    for (int bit = 0; bit < 64; ++bit) {
        const auto power = std::uint64_t{1} << bit;
        CHECK(dftu_floor_pow2_u64(power) == power);
        CHECK(dftu_ceil_pow2_u64(power) == power);
    }
    CHECK(dftu_ceil_pow2_u64(std::numeric_limits<std::uint64_t>::max()) == 0);
    CHECK(dftu_mul_hi_u64(UINT64_MAX, UINT64_MAX) == UINT64_MAX - 1);
    CHECK(dftu_isqrt_u64(UINT64_MAX) == UINT64_C(0xffffffff));
    CHECK(dftu_sqrt_f64(81.0) == 9.0);
    CHECK(dftu_log2_f64(16.0) == 4.0);
    CHECK(dftu_fma_f64(2.0, 3.0, 4.0) == 10.0);
}
