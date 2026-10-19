#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/binaries/json_cell_printer.h>
#include <doctest/doctest.h>

#include <array>
#include <cstdint>

using dftracer::utils::binaries::detail::decimal_to_exact_string;

TEST_CASE("CLI decimals retain all 128-bit and 256-bit digits") {
    std::array<std::uint8_t, 16> small{};
    CHECK(decimal_to_exact_string(small.data(), small.size(), 2) == "0.00");
    small[0] = 123;
    CHECK(decimal_to_exact_string(small.data(), small.size(), 2) == "1.23");
    small.fill(0xff);
    CHECK(decimal_to_exact_string(small.data(), small.size(), 2) == "-0.01");
    small.back() = 0x7f;
    CHECK(decimal_to_exact_string(small.data(), small.size(), 0) ==
          "170141183460469231731687303715884105727");
    small.fill(0);
    small.back() = 0x80;
    CHECK(decimal_to_exact_string(small.data(), small.size(), 0) ==
          "-170141183460469231731687303715884105728");

    std::array<std::uint8_t, 32> large;
    large.fill(0xff);
    large.back() = 0x7f;
    CHECK(decimal_to_exact_string(large.data(), large.size(), 0) ==
          "57896044618658097711785492504343953926634992332820282019728792003956"
          "564819967");
    large.fill(0);
    large.back() = 0x80;
    CHECK(decimal_to_exact_string(large.data(), large.size(), 0) ==
          "-5789604461865809771178549250434395392663499233282028201972879200395"
          "6564819968");
}
