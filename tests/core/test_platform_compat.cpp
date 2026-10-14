#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/platform_compat.h>
#include <doctest/doctest.h>
#include <sys/stat.h>

#include <cstdio>

TEST_CASE("path and descriptor stats agree and retain 64-bit file sizes") {
    struct stat path_stat{};
    struct stat fd_stat{};
    static_assert(sizeof(path_stat.st_size) >= 8);
    REQUIRE(::stat(__FILE__, &path_stat) == 0);

    FILE* file = std::fopen(__FILE__, "rb");
    REQUIRE(file != nullptr);
    const int result = ::fstat(::fileno(file), &fd_stat);
    std::fclose(file);
    REQUIRE(result == 0);
    CHECK(fd_stat.st_size == path_stat.st_size);
    CHECK(fd_stat.st_size > 0);
}
