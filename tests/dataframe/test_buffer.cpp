#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/buffer.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>

using dftracer::utils::dataframe::Buffer;

TEST_CASE("allocated buffers keep their byte size and requested alignment") {
    for (std::size_t align :
         {std::size_t{64}, std::size_t{128}, std::size_t{4096}}) {
        for (std::size_t bytes :
             {std::size_t{1}, std::size_t{65}, std::size_t{8193}}) {
            CAPTURE(align);
            CAPTURE(bytes);
            auto buffer = Buffer::allocate(bytes, align);
            REQUIRE(buffer->data() != nullptr);
            CHECK(buffer->size() == bytes);
            CHECK(reinterpret_cast<std::uintptr_t>(buffer->data()) % align ==
                  0);
            buffer->data()[0] = 17;
            buffer->data()[bytes - 1] = 23;
            CHECK(buffer->data()[bytes - 1] == 23);
        }
    }
}

TEST_CASE("empty buffers have no allocation") {
    auto buffer = Buffer::allocate(0);
    CHECK(buffer->size() == 0);
    CHECK(buffer->data() == nullptr);
}

TEST_CASE("foreign memory is released once after the final buffer owner") {
    std::uint8_t data = 42;
    int releases = 0;
    auto buffer = Buffer::wrap(&data, 1, [&](void* ptr) {
        CHECK(ptr == &data);
        ++releases;
    });
    auto shared = buffer;
    buffer.reset();
    CHECK(releases == 0);
    CHECK(shared->data()[0] == 42);
    shared.reset();
    CHECK(releases == 1);
}
