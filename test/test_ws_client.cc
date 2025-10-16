#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "camera.h"
#include "ws_client.h"

using json = nlohmann::json;

int main(int argc, char **argv)
{
    auto client = xvc::ws_client("192.168.177.100", "8000", [](std::string_view event) {
        auto const device_event = json::parse(event);
        auto const event_type = device_event["event_type"];
        auto const camera_json = device_event["camera"];
        spdlog::info("event_type = {}", event_type.get<std::string_view>());

        if (event_type == "Added") {
            auto cameras = Camera::parse(camera_json);
            spdlog::info("Added camera name = {}", cameras->name());
        } else if (event_type == "Removed") {
            auto const id = camera_json["id"].get<int>();
            spdlog::info("Removed camera id = {}", id);
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(60));
}