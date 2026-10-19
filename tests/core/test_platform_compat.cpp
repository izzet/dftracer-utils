#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/io/platform_io.h>
#include <doctest/doctest.h>
#include <sys/stat.h>

#include <cstdio>

TEST_CASE("path and descriptor stats agree and retain 64-bit file sizes") {
    dftracer::utils::io::FileStat path_stat{};
    dftracer::utils::io::FileStat fd_stat{};
    static_assert(sizeof(path_stat.st_size) >= 8);
    REQUIRE(dftracer::utils::io::platform_io::stat_path(__FILE__, &path_stat) ==
            0);

    FILE* file = std::fopen(__FILE__, "rb");
    REQUIRE(file != nullptr);
    const int result =
        dftracer::utils::io::platform_io::stat_fd(::fileno(file), &fd_stat);
    std::fclose(file);
    REQUIRE(result == 0);
    CHECK(fd_stat.st_size == path_stat.st_size);
    CHECK(fd_stat.st_size > 0);
}
