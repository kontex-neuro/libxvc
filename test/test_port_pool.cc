#include <fmt/core.h>

#include "port_pool.h"

int main()
{
    try {
        PortPool pool1(9000, 9010);
        PortPool pool2(9000, 9010);

        pool1.print_available_ports();
        pool2.print_available_ports();

        auto port1 = pool1.allocate_port();
        auto port2 = pool2.allocate_port();

        pool1.print_available_ports();
        pool1.release_port(port1.value());

        pool2.print_available_ports();
        pool2.release_port(port2.value());

        std::vector<PortPool::Port> allocated1;
        try {
            for (auto i = 0; i < 20; ++i) {
                auto port = pool1.allocate_port();
                if (!port) continue;
                allocated1.push_back(port.value());
            }
        } catch (const std::exception &ex) {
            fmt::println("Expected exception on exhaustion: {}", ex.what());
        }

        std::vector<PortPool::Port> allocated2;
        try {
            for (auto i = 0; i < 20; ++i) {
                auto port = pool2.allocate_port();
                if (!port) continue;
                allocated2.push_back(port.value());
            }
        } catch (const std::exception &ex) {
            fmt::println("Expected exception on exhaustion: {}", ex.what());
        }

    } catch (const std::exception &ex) {
        fmt::println("Test failed: {}", ex.what());
    }

    return 0;
}


// #include <catch2/catch_session.hpp>
// #include <catch2/catch_test_macros.hpp>

// #include "port_pool.h"

// TEST_CASE("PortPool allocates a valid port", "[allocate]")
// {
//     PortPool pool(40000, 40010);
//     auto port = pool.allocate_port();

//     REQUIRE(port.has_value());
//     REQUIRE(port.value() >= 40000);
//     REQUIRE(port.value() < 40010);
// }

// TEST_CASE("PortPool allocates all ports then fails", "[allocate][exhaust]")
// {
//     const int start = 40100;
//     const int end = 40105;
//     PortPool pool(start, end);

//     std::set<PortPool::Port> allocated_ports;
//     for (int i = 0; i < (end - start); ++i) {
//         auto port = pool.allocate_port();
//         REQUIRE(port.has_value());
//         allocated_ports.insert(port.value());
//     }

//     REQUIRE(allocated_ports.size() == static_cast<size_t>(end - start));

//     SECTION("No ports should be available now")
//     {
//         auto port = pool.allocate_port();
//         REQUIRE_FALSE(port.has_value());
//     }
// }

// TEST_CASE("PortPool releases and reallocates a port", "[release][reuse]")
// {
//     PortPool pool(40200, 40203);

//     auto port1 = pool.allocate_port();
//     REQUIRE(port1.has_value());

//     pool.release_port(port1.value());

//     auto port2 = pool.allocate_port();
//     REQUIRE(port2.has_value());
//     REQUIRE(port2.value() == port1.value());
// }

// TEST_CASE("PortPool rejects invalid range", "[ctor]")
// {
//     REQUIRE_THROWS_AS(PortPool(5000, 5000), std::invalid_argument);
//     REQUIRE_THROWS_AS(PortPool(5001, 5000), std::invalid_argument);
// }

// TEST_CASE("PortPool handles out-of-range releases safely", "[release]")
// {
//     PortPool pool(40300, 40302);

//     REQUIRE_NOTHROW(pool.release_port(40299));
//     REQUIRE_NOTHROW(pool.release_port(40302));  // Equal to end
//     REQUIRE_NOTHROW(pool.release_port(50000));  // Far outside
// }
