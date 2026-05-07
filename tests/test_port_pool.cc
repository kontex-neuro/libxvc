#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <set>

#include "port_pool.h"

TEST_CASE("PortPool constructs with valid range", "[port_pool]")
{
    SECTION("Constructs with valid range") { REQUIRE_NOTHROW(PortPool{9000, 9005}); }

    SECTION("Allocates unique ports within range")
    {
        PortPool pool(9000, 9005);
        std::set<PortPool::Port> allocated;
        for (auto i = 0; i < 5; ++i) {
            auto port = pool.allocate();
            REQUIRE(port.has_value());
            REQUIRE(port.value() >= 9000);
            REQUIRE(port.value() < 9005);
            REQUIRE(allocated.insert(port.value()).second);
        }
    }

    SECTION("Failed to allocate when pool is exhausted")
    {
        PortPool pool(9000, 9002);
        REQUIRE(pool.allocate());
        REQUIRE(pool.allocate());
        REQUIRE_FALSE(pool.allocate());
    }

    SECTION("Released ports can be reallocated")
    {
        PortPool pool(9000, 9002);

        auto p1 = pool.allocate();
        auto p2 = pool.allocate();
        REQUIRE(p1);
        REQUIRE(p2);

        pool.release(p1.value());
        auto p3 = pool.allocate();
        REQUIRE(p3);

        REQUIRE(p3.value() == p1.value());
    }

    SECTION("Releasing port which is out of range is safe")
    {
        PortPool pool(9000, 9002);
        REQUIRE_FALSE(pool.release(9002));
    }
}
