#include <spdlog/spdlog.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "camera.h"
#include "ws_client.h"

using namespace std::chrono_literals;

TEST_CASE("WebSocket add/remove cameras", "[ws][camera]")
{
    std::atomic<bool> received_event{false};
    std::vector<std::unique_ptr<Camera>> cameras;

    auto client = xvc::ws_client("192.168.177.100", "8000", [&](std::string event) {
        REQUIRE_NOTHROW(nlohmann::json::parse(event));

        auto const device_event = nlohmann::json::parse(event);
        REQUIRE(device_event.contains("event_type"));
        REQUIRE(device_event.contains("camera"));

        auto const &event_type = device_event.at("event_type").get<std::string>();
        auto const &camera_json = device_event.at("camera");

        if (event_type == "Added") {
            auto camera = Camera::parse(camera_json.dump());
            REQUIRE(camera);

            cameras.emplace_back(std::move(camera));
            received_event = true;

        } else if (event_type == "Removed") {
            REQUIRE(camera_json.contains("id"));

            auto const id = camera_json.at("id").get<int>();
            cameras.erase(
                std::remove_if(
                    cameras.begin(),
                    cameras.end(),
                    [id](const std::unique_ptr<Camera> &cam) { return cam->id() == id; }
                ),
                cameras.end()
            );
            received_event = true;

        } else {
            FAIL("Unknown event_type: " + event_type);
        }
    });

    std::this_thread::sleep_for(10s);
    REQUIRE(received_event);
}
