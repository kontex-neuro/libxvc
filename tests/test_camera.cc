#include <catch2/catch_test_macros.hpp>

#include "camera.h"

TEST_CASE("Camera::parse", "[camera][parse]")
{
    SECTION("Parses valid camera JSON")
    {
        constexpr auto json = R"(
        {
            "id": 1,
            "device_id": "0000:XXXX",
            "name": "Test Camera",
            "caps": [
                {
                    "media_type": "video/x-h264",
                    "format": "H264",
                    "width": 1920,
                    "height": 1080,
                    "framerate": "30/1"
                }
            ]
        }
        )";
        auto camera = Camera::parse(json);
        REQUIRE(camera);
    }

    SECTION("Rejects non-object JSON")
    {
        constexpr auto json = R"(
        [
            {
                "id": 1,
                "device_id": "0000:XXXX",
                "name": "Test Camera",
                "caps": [
                    {
                        "media_type": "video/x-h264",
                        "format": "H264",
                        "width": 1920,
                        "height": 1080,
                        "framerate": "30/1"
                    }
                ]
            }
        ]
        )";
        auto camera = Camera::parse(json);
        REQUIRE_FALSE(camera);
    }

    SECTION("Rejects JSON missing required field")
    {
        constexpr auto json = R"(
        {
            "device_id": "0000:XXXX",
            "name": "Test Camera",
            "caps": [
                {
                    "media_type": "video/x-h264",
                    "format": "H264",
                    "width": 1920,
                    "height": 1080,
                    "framerate": "30/1"
                }
            ]
        }
        )";
        auto camera = Camera::parse(json);
        REQUIRE_FALSE(camera);
    }
}

TEST_CASE("Camera port allocation does not collide", "[camera][portpool]")
{
    using namespace std::chrono_literals;

    SECTION("Exhausting port pool of camera throws runtime_error")
    {
        std::vector<std::unique_ptr<Camera>> cameras;
        const auto size = 64;
        cameras.reserve(size);
        for (auto i = 0; i < size; ++i) {
            auto cam = std::make_unique<Camera>(0, "0000:XXXX", "cam");
            cameras.push_back(std::move(cam));
        }
        REQUIRE_THROWS_AS(Camera(0, "0000:XXXX", "cam"), std::runtime_error);
    }

    SECTION("Port is freed when camera is destroyed")
    {
        auto cam1 = std::make_unique<Camera>(0, "0000:XXXX", "cam");
        auto port1 = cam1->port();
        cam1.reset();
        auto cam2 = std::make_unique<Camera>(0, "0000:XXXX", "cam");
        auto port2 = cam2->port();

        REQUIRE(port2 == port1);
    }
}